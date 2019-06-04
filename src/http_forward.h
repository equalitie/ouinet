#pragma once

#include <array>

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/parser.hpp>

#include "full_duplex_forward.h"
#include "or_throw.h"
#include "util.h"
#include "util/signal.h"
#include "util/yield.h"

#include "namespaces.h"

namespace ouinet {

template<class StreamIn, class StreamOut, class Request>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , Request rq
            , Cancel& cancel
            , Yield yield_)
{
    // TODO: Split and refactor with `fetch_http` if still useful.
    using ResponseH = http::response_header<>;

    Yield yield = yield_.tag("http_forward");

    auto cancelled = cancel.connect([&] { in.close(); out.close(); });

    sys::error_code ec;

    // Send the HTTP request to the input side.
    http::async_write(in, rq, yield[ec]);
    // Ignore `end_of_stream` error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream)
        ec = sys::error_code();

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to send request: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    beast::flat_buffer buffer;

    // Receive the head of the HTTP response into a parser.
    http::response_parser<http::empty_body> rpp;
    http::async_read_header(in, buffer, rpp, yield[ec]);
    if (buffer.size() > half_duplex_default_block)
        ec = asio::error::message_size;  // TODO: better handling

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to receive response head: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    // Cut buffer size down to the one that will be used in forwarding.
#if BOOST_VERSION >= 107000
    buffer.max_size(half_duplex_default_block);
    auto& buffer_ = buffer;
#else
    std::array<uint8_t, half_duplex_default_block> data;
    auto buffer_ = asio::buffer(data);
    asio::buffer_copy(buffer_, buffer.data());
#endif

    assert(rpp.is_header_done());
    auto rph = rpp.get();
    assert(!rph.chunked());  // TODO: implement

    // Get content length.
    static const auto max_size_t = std::numeric_limits<std::size_t>::max();
    auto content_length = rph[http::field::content_length];
    size_t max_transfer;
    if (content_length.empty())
        ec = asio::error::operation_not_supported;
    if (!ec)
        max_transfer = util::parse_num<size_t>(content_length, max_size_t);
    if (!ec && max_transfer == max_size_t)
        ec = asio::error::invalid_argument;
    if (ec) return or_throw<ResponseH>(yield, ec);

    // TODO: Allow processing the parsed head before writing.

    // Send the HTTP response head.
    yield.log("=== Sending back response ===");  // TODO: log while processing
    yield.log(rph.base());
    http::async_write(out, rph, yield[ec]);

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to send response head: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    // Forward the body.
    half_duplex(in, out, buffer_, max_transfer, yield[ec]);

    if (!ec && cancelled)
        ec = asio::error::operation_aborted;
    if (ec) yield.log("Failed to forward response body: ", ec.message());
    if (ec) return or_throw<ResponseH>(yield, ec);

    return rpp.release().base();
}

} // namespace ouinet
