#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/format.hpp>

#include "namespaces.h"
#include "generic_stream.h"
#include "or_throw.h"
#include "response_part.h"
#include "util/yield.h"
#include "util/variant.h"


namespace ouinet { namespace http_response {

void
Part::async_write( GenericStream& con
                 , Cancel cancel
                 , asio::yield_context yield) const
{
    auto cancelled = cancel.connect([&] { con.close(); });

    sys::error_code ec;

    util::apply(*this,
        [&] (const http_response::Head& head) {
            Head::writer headw(head, head.version(), head.result_int());
            asio::async_write(con, headw.get(), yield[ec]);
        },
        [&] (const http_response::Body& body) {
            asio::async_write(con, asio::buffer(body), yield[ec]);
        },
        [&] (const http_response::ChunkHdr& chunk_hdr) {
            if (chunk_hdr.size > 0)
                asio::async_write(con
                                 , http::chunk_header{ chunk_hdr.size
                                                     , chunk_hdr.exts}
                                 , yield[ec]);
            else {  // `http::chunk_last` carries a trailer itself, do not use
                static const auto hdrf = "0%s\r\n";
                auto hdr = (boost::format(hdrf) % chunk_hdr.exts).str();
                asio::async_write(con, asio::buffer(hdr), yield[ec]);
            }
        },
        [&] (const http_response::ChunkBody& chunk_body) {
            asio::async_write(con, asio::buffer(chunk_body), yield[ec]);
            if (chunk_body.remain == 0)
                asio::async_write(con, http::chunk_crlf{}, yield[ec]);
        },
        [&] (const http_response::Trailer& trailer) {
            Trailer::writer trailerw(trailer);
            asio::async_write(con, trailerw.get(), yield[ec]);
        });

    if (cancelled) ec = asio::error::operation_aborted;

    return or_throw(yield, ec);
}

}} // namespace ouinet::http_response
