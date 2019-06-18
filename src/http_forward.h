#pragma once

#include <array>

#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

#include "default_timeout.h"
#include "defer.h"
#include "http_util.h"
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

// Get copy of response trailers from input, return response trailers for output.
// Only trailers declared in the input response's `Trailers:` header are considered.
using ProcTrailFunc = std::function<http::fields(http::fields, Cancel&, Yield)>;

// Send the HTTP request `rq` over `in`, send the response head over `out`,
// then forward the response body from `in` to `out`.
//
// The `rshproc` callback can be used to manipulate the response head
// before sending it to `out`.
// It can be used to set output transfer encoding to chunked.
//
// The `inproc` callback can be used to manipulate blocks of input
// (of at most `http_forward_block` size)
// before sending the resulting data to `out`.
// Every non-empty result is sent in a single write operation
// (wrapped in a single chunk if the output is chunked).
//
// The `trproc` callback can be used to manipulate trailers
// before sending them to `out`.
template<class StreamIn, class StreamOut, class Request, class ConstBufferSequence>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , Request rq
            , ProcHeadFunc rshproc
            , ProcInFunc<ConstBufferSequence> inproc
            , ProcTrailFunc trproc
            , Cancel& cancel
            , Yield yield_)
{
    // TODO: Split and refactor with `fetch_http` if still useful.
    using ResponseH = http::response_header<>;
    static const auto max_size_t = (std::numeric_limits<std::size_t>::max)();

    Yield yield = yield_.tag("http_forward");

    // Cancellation, time out and error handling
    // -----------------------------------------
    auto cancelled = cancel.connect([&] { in.close(); out.close(); });
    bool timed_out = false;
    auto wdog_timeout = default_timeout::http_forward();
    WatchDog wdog( in.get_io_service(), wdog_timeout
                 , [&] { timed_out = true; in.close(); out.close(); });

    sys::error_code ec;
    auto set_error = [&] (sys::error_code& ec, const auto& msg) {
        if (cancelled) ec = asio::error::operation_aborted;
        else if (timed_out) ec = asio::error::timed_out;
        if (ec) yield.log(msg, ": ", ec.message());
        return ec;
    };

    // Send HTTP request to input side
    // -------------------------------
    http::async_write(in, rq, yield[ec]);
    // Ignore `end_of_stream` error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream)
        ec = sys::error_code();
    if (set_error(ec, "Failed to send request"))
        return or_throw<ResponseH>(yield, ec);

    // Receive HTTP response head from input side and parse it
    // -------------------------------------------------------
    beast::static_buffer<http_forward_block> inbuf;
    http::response_parser<http::empty_body> rpp;
    rpp.body_limit(max_size_t);  // i.e. unlimited; callbacks can restrict this
    http::async_read_header(in, inbuf, rpp, yield[ec]);
    if (set_error(ec, "Failed to receive response head"))
        return or_throw<ResponseH>(yield, ec);

    wdog.expires_after(wdog_timeout);

    assert(rpp.is_header_done());
    auto rp = rpp.get();
    bool chunked_in = rp.chunked();

    // Get content length if non-chunked.
    size_t nc_pending;
    bool http_10_eob = false;  // HTTP/1.0 end of body on connection close, no `Content-Length`
    if (!chunked_in) {
        nc_pending = util::parse_num<size_t>( rp[http::field::content_length]
                                            , max_size_t);
        if (nc_pending == max_size_t) {
            if (rp.version() == 10)
                http_10_eob = true;
            else
                return or_throw<ResponseH>(yield, asio::error::invalid_argument);
        }
    }

    // Process and send HTTP response head to output side
    // --------------------------------------------------
    bool chunked_out;
    {
        auto rph_out(rpp.get().base());
        rph_out = rshproc(std::move(rph_out), cancel, yield[ec]);
        if (set_error(ec, "Failed to process response head"))
            return or_throw<ResponseH>(yield, ec);

        chunked_out = http::response<http::empty_body>(rph_out).chunked();
        assert(!(chunked_in && !chunked_out));  // implies slurping response into memory

        // Write the head as a string to avoid the serializer adding an empty body
        // (which results in a terminating chunk if chunked).
        auto rph_outs = util::str(rph_out);
        asio::async_write(out, asio::buffer(rph_outs.data(), rph_outs.size()), yield[ec]);
    }
    if (set_error(ec, "Failed to send response head"))
        return or_throw<ResponseH>(yield, ec);

    wdog.expires_after(wdog_timeout);

    // Process and forward body blocks
    // -------------------------------
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

            if (ec == asio::error::eof && http_10_eob) {
                ec = {};  // HTTP/1.0 end of body as of RFC1945#7.2.2
                nc_pending = 0;
            }
        }
        if (set_error(ec, "Failed to read response body"))
            break;

        ConstBufferSequence outbuf = inproc(fwdbuf, cancel, yield[ec]);
        if (set_error(ec, "Failed to process response body"))
            break;
        if (asio::buffer_size(outbuf) == 0)
            continue;  // e.g. input buffer filled but no output yet

        if (chunked_out)
            asio::async_write(out, http::make_chunk(outbuf), yield[ec]);
        else
            asio::async_write(out, outbuf, yield[ec]);
        if (set_error(ec, "Failed to send response body"))
            break;
    }
    if (ec) return or_throw<ResponseH>(yield, ec);

    auto rph = rpp.release().base();

    // Process and send last chunk and trailers to output side
    // -------------------------------------------------------
    if (chunked_out) {
        http::fields intrail;
        for (const auto& hdr : http::token_list(rph[http::field::trailer])) {
            auto hit = rph.find(hdr);
            if (hit == rph.end())
                continue;  // missing trailer
            intrail.insert(hit->name(), hit->value());
        }

        auto outtrail = trproc(std::move(intrail), cancel, yield[ec]);
        if (set_error(ec, "Failed to process response trailers"))
            return or_throw<ResponseH>(yield, ec);

        if (outtrail.begin() != outtrail.end())
            asio::async_write(out, http::make_chunk_last(outtrail), yield[ec]);
        else
            asio::async_write(out, http::make_chunk_last(), yield[ec]);
        if (set_error(ec, "Failed to send last chunk and trailers"))
            return or_throw<ResponseH>(yield, ec);
    }

    return rph;
}

} // namespace ouinet
