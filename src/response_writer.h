#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/format.hpp>

#include "generic_stream.h"
#include "namespaces.h"
#include "or_throw.h"
#include "response_part.h"
#include "util/signal.h"
#include "util/yield.h"
#include "util/variant.h"


namespace ouinet { namespace http_response {

class Writer {
public:
    Writer(GenericStream out);

    void async_write_part(const Part&, Cancel, asio::yield_context);

private:
    GenericStream _out;
    Cancel _lifetime_cancel;
};


Writer::Writer(GenericStream out)
    : _out(std::move(out))
{
}

void
Writer::async_write_part(const Part& part, Cancel cancel, asio::yield_context yield)
{
    namespace Err = asio::error;

    // Cancellation, time out and error handling
    auto lifetime_cancelled = _lifetime_cancel.connect([&] { cancel(); });
    auto cancelled = cancel.connect([&] { _out.close(); });

    sys::error_code ec;

    auto set_error = [&] (sys::error_code& ec, const auto& msg) {
        if (cancelled) ec = Err::operation_aborted;
        return ec;
    };

    util::apply(part,
        [&] (const http_response::Head& head) {
            Head::writer headw(head, head.version(), head.result_int());
            asio::async_write(_out, headw.get(), yield[ec]);
        },
        [&] (const http_response::Body& body) {
            asio::async_write(_out, asio::buffer(body), yield[ec]);
        },
        [&] (const http_response::ChunkHdr& chunk_hdr) {
            if (chunk_hdr.size > 0)
                asio::async_write(_out
                                 , http::chunk_header{ chunk_hdr.size
                                                     , chunk_hdr.exts}
                                 , yield[ec]);
            else {  // `http::chunk_last` carries a trailer itself, do not use
                static const auto hdrf = "0%s\r\n";
                auto hdr = (boost::format(hdrf) % chunk_hdr.exts).str();
                asio::async_write(_out, asio::buffer(hdr), yield[ec]);
            }
        },
        [&] (const http_response::ChunkBody& chunk_body) {
            asio::async_write(_out, asio::buffer(chunk_body), yield[ec]);
            if (chunk_body.remain == 0)
                asio::async_write(_out, http::chunk_crlf{}, yield[ec]);
        },
        [&] (const http_response::Trailer& trailer) {
            Trailer::writer trailerw(trailer);
            asio::async_write(_out, trailerw.get(), yield[ec]);
        });

    if (set_error(ec, "Failed to send response part"))
        return or_throw(yield, ec);
}

}} // namespace ouinet::http_response
