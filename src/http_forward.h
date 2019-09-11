#pragma once

#include <utility>
#include <vector>

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
#include <boost/utility/string_view.hpp>

#include "default_timeout.h"
#include "defer.h"
#include "http_util.h"
#include "or_throw.h"
#include "util/chunk_last_x.h"
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
//
// If a non-empty string is returned along the data,
// it is attached as chunk extensions to the chunk to be sent
// (only if chunked transfer encoding is enabled at the output).
template<class ConstBufferSequence>
using ProcInFunc = std::function<
    std::pair<ConstBufferSequence, std::string>(asio::const_buffer inbuf, Cancel&, Yield)>;

// Get copy of response trailers from input, return response trailers for output.
// Only trailers declared in the input response's `Trailers:` header are considered.
//
// If a non-empty string is returned along the trailers,
// it is attached as chunk extensions to the last chunk to be sent.
using ProcTrailFunc = std::function<
    std::pair<http::fields, std::string>(http::fields, Cancel&, Yield)>;

// Notify about the reception of chunk extensions.
using ProcChkExtFunc = std::function<void(std::string, Cancel&, Yield)>;

namespace detail {
static const auto max_size_t = (std::numeric_limits<std::size_t>::max)();

std::string
process_head( const http::response_header<>&, const ProcHeadFunc&, bool& chunked_out
            , Cancel&, Yield);

std::pair<http::fields, std::string>
process_trailers( const http::response_header<>&, const ProcTrailFunc&
                , Cancel&, Yield);
}

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
//
// The `cxproc` callback is called whenever a non-empty chunk extension
// is received.
template<class StreamIn, class StreamOut, class Request, class ConstBufferSequence>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , Request rq
            , ProcHeadFunc rshproc
            , ProcInFunc<ConstBufferSequence> inproc
            , ProcTrailFunc trproc
            , ProcChkExtFunc cxproc
            , Cancel& cancel
            , Yield yield)
{
    auto cancelled = cancel.connect([&] { in.close(); out.close(); });
    bool timed_out = false;
    sys::error_code ec;

    // Send HTTP request to input side
    // -------------------------------
    {
        WatchDog wdog( in.get_io_service(), default_timeout::http_forward()
                     , [&] { timed_out = true; in.close(); out.close(); });
        http::async_write(in, rq, yield[ec]);
    }
    // Ignore `end_of_stream` error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream)
        ec = sys::error_code();
    if (timed_out)
        ec = asio::error::timed_out;
    if (cancelled)
        ec = asio::error::operation_aborted;
    if (ec) {
        yield.log("Failed to send request: ", ec.message());
        return or_throw<http::response_header<>>(yield, ec);
    }

    // Forward the response
    // --------------------
    return http_forward( in, out
                       , std::move(rshproc), std::move(inproc)
                       , std::move(trproc), std::move(cxproc)
                       , cancel, yield);
}

// Just as above, but assume that the request has already been sent.
template<class StreamIn, class StreamOut, class ConstBufferSequence>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , ProcHeadFunc rshproc
            , ProcInFunc<ConstBufferSequence> inproc
            , ProcTrailFunc trproc
            , ProcChkExtFunc cxproc
            , Cancel& cancel
            , Yield yield)
{
    beast::static_buffer<http_forward_block> inbuf;
    http::response_parser<http::empty_body> rpp;
    rpp.body_limit(detail::max_size_t);  // i.e. unlimited; callbacks can restrict this

    return http_forward( in, out, inbuf, rpp
                       , std::move(rshproc), std::move(inproc)
                       , std::move(trproc), std::move(cxproc)
                       , cancel, yield);
}

// Low-level version using an external buffer and response parser
// (which may have already processed the response head).
template< class StreamIn, class StreamOut
        , class InputBuffer, class ResponseParser
        , class ConstBufferSequence>
inline
http::response_header<>
http_forward( StreamIn& in
            , StreamOut& out
            , InputBuffer& inbuf
            , ResponseParser& rpp
            , ProcHeadFunc rshproc
            , ProcInFunc<ConstBufferSequence> inproc
            , ProcTrailFunc trproc
            , ProcChkExtFunc cxproc
            , Cancel& cancel
            , Yield yield_)
{
    // TODO: Split and refactor with `fetch_http` if still useful.
    using ResponseH = http::response_header<>;

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

    // Receive HTTP response head from input side and parse it
    // -------------------------------------------------------
    if (!rpp.is_header_done()) {
        http::async_read_header(in, inbuf, rpp, yield[ec]);
        if (set_error(ec, "Failed to receive response head"))
            return or_throw<ResponseH>(yield, ec);
    }

    assert(rpp.is_header_done());
    bool chunked_in = rpp.chunked();

    // Get content length if non-chunked.
    size_t nc_pending;
    bool http_10_eob = false;  // HTTP/1.0 end of body on connection close, no `Content-Length`
    if (!chunked_in) {
        auto clen = rpp.content_length();
        if (clen)
            nc_pending = *clen;
        else if (rpp.get().version() == 10)
            http_10_eob = true;
        else
            return or_throw<ResponseH>(yield, asio::error::invalid_argument);
    }

    wdog.expires_after(wdog_timeout);

    // Process and send HTTP response head to output side
    // --------------------------------------------------
    bool chunked_out;
    {
        auto outh = detail::process_head( rpp.get().base(), rshproc, chunked_out
                                        , cancel, yield[ec]);
        if (set_error(ec, "Failed to process response head"))
            return or_throw<ResponseH>(yield, ec);

        assert(!(chunked_in && !chunked_out));  // implies slurping response into memory
        asio::async_write(out, asio::buffer(outh), yield[ec]);
        if (set_error(ec, "Failed to send response head"))
            return or_throw<ResponseH>(yield, ec);
    }

    wdog.expires_after(wdog_timeout);

    // Process and forward body blocks and chunk extensions
    // ----------------------------------------------------
    // Based on "Boost.Beast / HTTP / Chunked Encoding / Parsing Chunks" example.

    // Prepare fixed-size forwarding buffer
    // (with body data already read for non-chunked input).
    std::vector<uint8_t> fwd_data(inbuf.max_size());
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

    std::string inexts;
    auto cx_cb = [&] (auto, auto exts, auto&) {
        // Just exfiltrate chunk extensions to be handled asynchronously.
        inexts = exts.to_string();
    };
    rpp.on_chunk_header(cx_cb);

    bool nc_done = false;
    while (chunked_in ? !rpp.is_done() : !nc_done) {
        auto reset_wdog = defer([&] { wdog.expires_after(wdog_timeout); });

        // Input buffer includes initial data on first read.
        if (chunked_in) {
            // Note this always produces a last empty read
            // to signal the end of input.
            fwdbuf = asio::buffer(fwd_data, 0);  // process empty if no body callback
            http::async_read(in, inbuf, rpp, yield[ec]);
            if (ec == http::error::end_of_chunk)
                ec = {};  // just a signal that we have input to process
        } else if (nc_pending <= 0) {
            // Arrange the explicit extra data processing call mentioned below
            // for non-chunked transfers.
            fwdbuf = {};
            nc_done = true;
        } else {
            // This does *not* produce a last empty read,
            // thus we need an extra data processing call with an empty buffer
            // to signal the end of input.
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

        if (!inexts.empty())
            cxproc(std::move(inexts), cancel, yield[ec]);
        if (set_error(ec, "Failed to process chunk extensions"))
            break;
        inexts = {};

        ConstBufferSequence outbuf; std::string outexts;
        std::tie(outbuf, outexts) = inproc(fwdbuf, cancel, yield[ec]);
        assert(chunked_out || !outexts.empty());  // must enable chunked output for extensions
        if (set_error(ec, "Failed to process response body"))
            break;
        if (asio::buffer_size(outbuf) == 0)
            continue;  // e.g. input buffer filled but no output yet

        if (chunked_out)
            asio::async_write(out, http::make_chunk(outbuf, outexts), yield[ec]);
        else
            asio::async_write(out, outbuf, yield[ec]);
        if (set_error(ec, "Failed to send response body"))
            break;
    }
    if (ec) return or_throw<ResponseH>(yield, ec);

    // Process and send last chunk and trailers to output side
    // -------------------------------------------------------
    auto rph = rpp.release().base();

    if (chunked_out) {
        http::fields outtrail; std::string outexts;
        std::tie(outtrail, outexts) = detail::process_trailers(rph, trproc, cancel, yield[ec]);
        if (set_error(ec, "Failed to process response trailers"))
            return or_throw<ResponseH>(yield, ec);

        if (outtrail.begin() != outtrail.end())
            asio::async_write( out, http::chunk_last_x<http::fields>(outexts, outtrail)
                             , yield[ec]);
        else
            asio::async_write( out, http::make_chunk_last_x(boost::string_view(outexts))
                             , yield[ec]);
        if (set_error(ec, "Failed to send last chunk and trailers"))
            return or_throw<ResponseH>(yield, ec);
    }

    return rph;
}

} // namespace ouinet
