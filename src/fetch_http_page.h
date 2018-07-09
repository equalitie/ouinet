#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "fail.h"
#include "or_throw.h"
#include "generic_connection.h"
#include "util/signal.h"
#include "util.h"
#include "connect_to_host.h"
#include "ssl/util.h"

namespace ouinet {

// Send the HTTP request `req` over the connection `con`
// (which may be already an SSL tunnel)
// *as is* and return the HTTP response.
template<class RequestType>
inline
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , GenericConnection& con
               , RequestType req
               , Signal<void()>& abort_signal
               , asio::yield_context yield)
{
    http::response<http::dynamic_body> res;
    return fetch_http(ios, con, req, move(res), abort_signal, yield);
}

// Send the HTTP request `req` over the connection `con`
// (which may be already an SSL tunnel)
// *as is* and return the HTTP response head.
template<class RequestType>
inline
http::response<http::empty_body>
fetch_http_head( asio::io_service& ios
               , GenericConnection& con
               , RequestType req
               , Signal<void()>& abort_signal
               , asio::yield_context yield)
{
    http::response<http::empty_body> res;
    return fetch_http(ios, con, req, move(res), abort_signal, yield);
}

template<class RequestType, class ResponseType>
inline
ResponseType
fetch_http( asio::io_service& ios
          , GenericConnection& con
          , RequestType req
          , ResponseType res
          , Signal<void()>& abort_signal
          , asio::yield_context yield)
{
    sys::error_code ec;

    auto close_con_slot = abort_signal.connect([&con] {
        con.close();
    });

    // Send the HTTP request to the remote host
    http::async_write(con, req, yield[ec]);

    // Ignore end_of_stream error, there may still be data in the receive
    // buffer we can read.
    if (ec == http::error::end_of_stream) {
        ec = sys::error_code();
    }

    if (ec) return or_throw(yield, ec, move(res));

    beast::flat_buffer buffer;

    // Receive the HTTP response
    recv_http_response(con, buffer, res, yield[ec]);

    return or_throw(yield, ec, move(res));
}

inline
void
recv_http_response( GenericConnection& con
                  , beast::flat_buffer& buffer
                  , http::response<http::dynamic_body>& res
                  , asio::yield_context yield)
{
    http::async_read(con, buffer, res, yield);
}

inline
void
recv_http_response( GenericConnection& con
                  , beast::flat_buffer& buffer
                  , http::response<http::empty_body>& res
                  , asio::yield_context yield)
{
    http::response_parser<http::empty_body> crph;
    http::async_read_header(con, buffer, crph, yield);
    res = move(crph.get());
}

// Retrieve the HTTP/HTTPS URL in the proxy request `req`
// (i.e. with a target like ``https://x.y/z``, not just ``/z``)
// *from the origin* and return the HTTP response.
template<class RequestType>
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , RequestType req
               , Signal<void()>& abort_signal
               , asio::yield_context yield)
{
    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;

    // Parse the URL to tell HTTP/HTTPS, host, port.
    util::url_match url;
    if (!util::match_http_url(req.target().to_string(), url)) {
        ec = asio::error::operation_not_supported;  // unsupported URL
        return or_throw<Response>(yield, ec);
    }
    bool ssl(url.scheme == "https");
    if (url.port.empty())
        url.port = ssl ? "443" : "80";

    auto con = connect_to_host(ios, url.host, url.port, abort_signal, yield[ec]);
    if (ec) return or_throw<Response>(yield, ec);

    return fetch_http_origin(ios, con, url, req, abort_signal, yield);
}

// Retrieve the pre-parsed HTTP/HTTPS `url` for the given proxy request `req`
// (i.e. with a target like ``https://x.y/z``, not just ``/z``)
// over an existing connection `con` *to the origin*
// (i.e. a direct connection or the tunnel resulting from HTTP CONNECT).
template<class RequestType>
http::response<http::dynamic_body>
fetch_http_origin( asio::io_service& ios
                 , GenericConnection& con
                 , const util::url_match& url
                 , RequestType req
                 , Signal<void()>& abort_signal
                 , asio::yield_context yield)
{
    using namespace std;
    using Response = http::response<http::dynamic_body>;

    auto target = req.target().to_string();
    sys::error_code ec;

    if (url.scheme == "https") {
        // Subsequent access to the connection will use the encrypted channel.
        con = ssl::util::client_handshake(move(con), url.host, yield[ec]);
        if (ec) {
            cerr << "SSL client handshake error: "
                 << url.host << ": " << ec.message() << endl;
            return or_throw<Response>(yield, ec);
        }
    }

    // Now that we have a connection to the origin
    // we can send a non-proxy request to it
    // (i.e. with target "/foo..." and not "http://example.com/foo...").
    // Actually some web servers do not like the full form.
    RequestType origin_req(req);
    origin_req.target(target.substr(target.find( url.path
                                               // Length of "http://" or "https://",
                                               // do not fail on "http(s)://FOO/FOO".
                                               , url.scheme.length() + 3)));
    return fetch_http_page(ios, con, origin_req, abort_signal, yield);
}

}
