#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>

#include "or_throw.h"
#include "util/signal.h"
#include "util/timeout.h"
#include "util/yield.h"
#include "util/watch_dog.h"
#include "util.h"
#include "connect_to_host.h"
#include "ssl/util.h"
#include "http_util.h"

namespace ouinet {

// Send the HTTP request `req` over the connection `con`
// (which may be already an SSL tunnel)
// *as is* and return the HTTP response or just its head
// depending on the expected `ResponseType`.
// Read but unused data may be left at the `buffer`.
template<class ResponseBodyType, class Stream, class RequestType, class DynamicBuffer>
inline
http::response<ResponseBodyType>
fetch_http( Stream& con
          , DynamicBuffer& buffer
          , RequestType req
          , Signal<void()>& abort_signal
          , Yield yield_)
{
    Yield yield = yield_.tag("fetch_http");

    http::response<ResponseBodyType> res;

    auto cancel_slot = abort_signal.connect([&con] { con.close(); });

    sys::error_code ec;

    // Send the HTTP request to the remote host
    http::async_write(con, req, yield[ec]);
    if (!ec && cancel_slot) {
        ec = asio::error::operation_aborted;
    }
    if (ec) {
        yield.log("Failed to http::async_write ", ec.message());
    }

    // Ignore end_of_stream error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream) {
        ec = sys::error_code();
    }

    if (ec) return or_throw(yield, ec, move(res));

    // Receive the HTTP response
    _recv_http_response(con, buffer, res, yield[ec]);
    if (!ec && cancel_slot) {
        ec = asio::error::operation_aborted;
    }
    if (ec) {
        yield.log("Failed to http::async_read ", ec.message());
    }

    return or_throw(yield, ec, move(res));
}

template<class ResponseBodyType, class Stream, class RequestType>
inline
http::response<ResponseBodyType>
fetch_http( Stream& con
          , RequestType req
          , Signal<void()>& abort_signal
          , Yield yield_)
{
    beast::flat_buffer buffer;
    return fetch_http<ResponseBodyType>(con, buffer, req, abort_signal, yield_);
}

template<class ResponseBodyType, class Stream, class Duration, class RequestType>
inline
http::response<ResponseBodyType>
fetch_http( asio::io_service& ios
          , Stream& con
          , RequestType req
          , Duration timeout
          , Signal<void()>& abort_signal
          , Yield yield)
{
    return util::with_timeout
        ( ios
        , abort_signal
        , timeout
        , [&] (auto& abort_signal, auto yield) {
              return fetch_http<ResponseBodyType>
                (con, req, abort_signal, yield);
          }
        , yield);
}

template<class Stream>
inline
void
_recv_http_response( Stream& con
                   , beast::flat_buffer& buffer
                   , http::response<http::dynamic_body>& res
                   , asio::yield_context yield)
{
    http::async_read(con, buffer, res, yield);
}

template<class Stream>
inline
void
_recv_http_response( Stream& con
                   , beast::flat_buffer& buffer
                   , http::response<http::empty_body>& res
                   , asio::yield_context yield)
{
    http::response_parser<http::empty_body> crph;
    http::async_read_header(con, buffer, crph, yield);
    res = move(crph.get());
}

} // namespace
