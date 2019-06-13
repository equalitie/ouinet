#pragma once

#include <array>

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/beast/http/parser.hpp>

#include "default_timeout.h"
#include "defer.h"
#include "or_throw.h"
#include "util.h"
#include "util/signal.h"
#include "util/watch_dog.h"
#include "util/yield.h"

#include "namespaces.h"

namespace ouinet {

static const size_t http_forward_block = 2048;

// Get copy of response head from input, return response head for output.
using ProcHeadFunc = std::function<
    http::response_header<>(http::response_header<>, Cancel&, Yield)>;
// Get a buffer of data to be sent after processing a buffer of received data.
// The returned data must be alive while `http_forward` runs,
// The returned data will be wrapped in a single chunk
// if the output response is chunked.
// If the received data is empty, no more data is to be received.
// If the returned buffer is empty, nothing is sent.
template<class ConstBufferSequence>
using ProcInFunc = std::function<
    ConstBufferSequence(asio::const_buffer inbuf, Cancel&, Yield)>;

template<class StreamIn, class StreamOut, class Request, class ConstBufferSequence>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , Request rq
            , ProcHeadFunc rshproc
            , ProcInFunc<ConstBufferSequence> inproc
            , Cancel& cancel
            , Yield yield_)
{
    // TODO: Split and refactor with `fetch_http` if still useful.
    using ResponseH = http::response_header<>;

    Yield yield = yield_.tag("http_forward");

    auto cancelled = cancel.connect([&] { in.close(); out.close(); });
    bool timed_out = false;
    auto wdog_timeout = default_timeout::http_forward();
    WatchDog wdog( in.get_io_service(), wdog_timeout
                 , [&] { timed_out = true; in.close(); out.close(); });

    sys::error_code ec;

    // Send the HTTP request to the input side.
    http::async_write(in, rq, yield[ec]);
    // Ignore `end_of_stream` error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream)
        ec = sys::error_code();
    if (timed_out) ec = asio::error::timed_out;
    if (cancelled) ec = asio::error::operation_aborted;
    if (ec) {
        yield.log("Failed to send request: ", ec.message());
        return or_throw<ResponseH>(yield, ec);
    }

    // Receive the head of the HTTP response into a parser.
    beast::static_buffer<http_forward_block> inbuf;
    http::response_parser<http::empty_body> rpp;
    http::async_read_header(in, inbuf, rpp, yield[ec]);
    if (timed_out) ec = asio::error::timed_out;
    if (cancelled) ec = asio::error::operation_aborted;
    if (ec) {
        yield.log("Failed to receive response head: ", ec.message());
        return or_throw<ResponseH>(yield, ec);
    }

    wdog.expires_after(wdog_timeout);

    assert(rpp.is_header_done());
    auto rp = rpp.get();
    bool chunked_in = rp.chunked();

    // Get content length if non-chunked.
    size_t nc_pending;
    if (!chunked_in) {
        static const auto max_size_t = std::numeric_limits<std::size_t>::max();
        nc_pending = util::parse_num<size_t>( rp[http::field::content_length]
                                            , max_size_t);
        if (nc_pending == max_size_t)
            return or_throw<ResponseH>(yield, asio::error::invalid_argument);
    }

    // Send the HTTP response head (after processing).
    bool chunked_out;
    {
        auto rph_out(rpp.get().base());
        rph_out = rshproc(std::move(rph_out), cancel, yield[ec]);
        if (timed_out) ec = asio::error::timed_out;
        if (cancelled) ec = asio::error::operation_aborted;
        if (ec) {
            yield.log("Failed to process response head: ", ec.message());
            return or_throw<ResponseH>(yield, ec);
        }

        chunked_out = http::response<http::empty_body>(rph_out).chunked();
        assert(!(chunked_in && !chunked_out));  // implies slurping response into memory

        // Write the head as a string to avoid the serializer adding an empty body
        // (which results in a terminating chunk if chunked).
        auto rph_outs = util::str(rph_out);
        asio::async_write(out, asio::buffer(rph_outs.data(), rph_outs.size()), yield[ec]);
    }
    if (timed_out) ec = asio::error::timed_out;
    if (cancelled) ec = asio::error::operation_aborted;
    if (ec) {
        yield.log("Failed to send response head: ", ec.message());
        return or_throw<ResponseH>(yield, ec);
    }

    wdog.expires_after(wdog_timeout);

    // Forward the body.
    // Based on "Boost.Beast / HTTP / Chunked Encoding / Parsing Chunks" example.

    // Prepare fixed-size forwarding buffer
    // (with body data already read for non-chunked input).
    std::array<uint8_t, http_forward_block> fwd_data;
    size_t fwd_initial;
    if (!chunked_in)
        fwd_initial = asio::buffer_copy(asio::buffer(fwd_data), inbuf.data());
    asio::mutable_buffer fwdbuf;

    auto body_cb = [&] (auto, auto body, auto& ec) {
        // Just exfiltrate a copy data for the input processing callback
        // to handle asynchronously
        // (we cannot be sure that the data in `body` will still be available
        // after the read operation returns).
        size_t length = body.size();
        fwdbuf = asio::buffer(fwd_data, length);
        asio::buffer_copy(fwdbuf, asio::const_buffer(body.data(), length));
        ec = http::error::end_of_chunk;  // not really, but similar semantics
        return length;
    };
    rpp.on_chunk_body(body_cb);

    while (chunked_in ? !rpp.is_done() : nc_pending > 0) {
        auto reset_wdog = defer([&] { wdog.expires_after(wdog_timeout); });

        // Input buffer includes initial data on first read.
        if (chunked_in) {
            http::async_read(in, inbuf, rpp, yield[ec]);
            if (ec == http::error::end_of_chunk)
                ec = {};  // just a signal that we have input to process
        } else {
            auto buf = asio::buffer(fwd_data, nc_pending);
            size_t length = fwd_initial + in.async_read_some(buf + fwd_initial, yield[ec]);
            fwd_initial = 0;  // only usable on first read
            nc_pending -= length;
            fwdbuf = asio::buffer(buf, length);
        }
        if (timed_out) ec = asio::error::timed_out;
        if (cancelled) ec = asio::error::operation_aborted;
        if (ec) {
           yield.log("Failed to read response body: ", ec.message());
           break;
        }

        ConstBufferSequence outbuf = inproc(fwdbuf, cancel, yield[ec]);
        if (timed_out) ec = asio::error::timed_out;
        if (cancelled) ec = asio::error::operation_aborted;
        if (ec) {
           yield.log("Failed to process response body: ", ec.message());
           break;
        }
        if (asio::buffer_size(outbuf) == 0)
           continue;  // e.g. input buffer filled but no output yet

        if (chunked_out)
            asio::async_write(out, http::make_chunk(outbuf), yield[ec]);
        else
            asio::async_write(out, outbuf, yield[ec]);
        if (timed_out) ec = asio::error::timed_out;
        if (cancelled) ec = asio::error::operation_aborted;
        if (ec) {
            yield.log("Failed to send response body: ", ec.message());
            break;
        }
    }

    if (!ec && chunked_out)
        // Trailers are handled outside.
        asio::async_write(out, http::make_chunk_last(), yield[ec]);

    if (timed_out) ec = asio::error::timed_out;
    if (cancelled) ec = asio::error::operation_aborted;
    if (ec) return or_throw<ResponseH>(yield, ec);

    return rpp.release().base();
}

} // namespace ouinet
