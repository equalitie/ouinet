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


namespace ouinet { namespace http_response {

class Writer {
public:
    Writer(GenericStream out);

    void async_write_part(const Part&, Cancel, Yield);

private:
    GenericStream _out;
    Cancel _lifetime_cancel;
};


Writer::Writer(GenericStream out)
    : _out(std::move(out))
{
}

void
Writer::async_write_part(const Part& part, Cancel cancel, Yield yield_)
{
    namespace Err = asio::error;

    Yield yield = yield_.tag("http_forward");

    // Cancellation, time out and error handling
    auto lifetime_cancelled = _lifetime_cancel.connect([&] { cancel(); });
    auto cancelled = cancel.connect([&] { _out.close(); });

    sys::error_code ec;

    auto set_error = [&] (sys::error_code& ec, const auto& msg) {
        if (cancelled) ec = Err::operation_aborted;
        if (ec) yield.log(msg, ": ", ec.message());
        return ec;
    };

    // Dumb approach, just write the parts as they come.
    // TODO: Implement a state machine that catches illegal writes.
    if (auto headp = part.as_head()) {
        Head::writer headw(*headp, headp->version(), headp->result_int());
        asio::async_write(_out, headw.get(), yield[ec]);
    } else if (auto bodyp = part.as_body()) {
        asio::async_write(_out, asio::buffer(*bodyp), yield[ec]);
    } else if (auto chunkhp = part.as_chunk_hdr()) {
        if (chunkhp->size > 0)
            asio::async_write(_out, http::chunk_header{chunkhp->size, chunkhp->exts}, yield[ec]);
        else {  // `http::chunk_last` carries a trailer itself, do not use
            static const auto hdrf = "0%s\r\n";
            auto hdr = (boost::format(hdrf) % chunkhp->exts).str();
            asio::async_write(_out, asio::buffer(hdr), yield[ec]);
        }
    } else if (auto chunkbp = part.as_chunk_body()) {
        asio::async_write(_out, asio::buffer(*chunkbp), yield[ec]);
        asio::async_write(_out, http::chunk_crlf{}, yield[ec]);
    } else if (auto trailerp = part.as_trailer()) {
        Trailer::writer trailerw(*trailerp);
        asio::async_write(_out, trailerw.get(), yield[ec]);
    } else {
        assert(0 && "Unknown response part");
    }

    if (set_error(ec, "Failed to send response part"))
        return or_throw(yield, ec);
}

}} // namespace ouinet::http_response
