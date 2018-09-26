#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "or_throw.h"
#include "generic_connection.h"
#include "util/signal.h"
#include "util/timeout.h"
#include "util.h"
#include "connect_to_host.h"
#include "ssl/util.h"

namespace ouinet {

// Send the HTTP request `req` over the connection `con`
// (which may be already an SSL tunnel)
// *as is* and return the HTTP response or just its head
// depending on the expected `ResponseType`.
template<class ResponseBodyType, class RequestType>
inline
http::response<ResponseBodyType>
fetch_http( asio::io_service& ios
          , GenericConnection& con
          , RequestType req
          , Signal<void()>& abort_signal
          , asio::yield_context yield)
{
    http::response<ResponseBodyType> res;

    auto slot = abort_signal.connect([&con] { con.close(); });

    sys::error_code ec;

    // Send the HTTP request to the remote host
    http::async_write(con, req, yield[ec]);

    // Ignore end_of_stream error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream) {
        ec = sys::error_code();
    }

    if (ec) return or_throw(yield, ec, move(res));

    beast::flat_buffer buffer;

    // Receive the HTTP response
    _recv_http_response(con, buffer, res, yield[ec]);

    return or_throw(yield, ec, move(res));
}

template<class ResponseBodyType, class Duration, class RequestType>
inline
http::response<ResponseBodyType>
fetch_http( asio::io_service& ios
          , GenericConnection& con
          , RequestType req
          , Duration timeout
          , Signal<void()>& abort_signal
          , asio::yield_context yield)
{
    return util::with_timeout
        ( ios
        , abort_signal
        , timeout
        , [&] (auto& abort_signal, auto yield) {
              return fetch_http<ResponseBodyType>
                (ios, con, req, abort_signal, yield);
          }
        , yield);
}

inline
void
_recv_http_response( GenericConnection& con
                   , beast::flat_buffer& buffer
                   , http::response<http::dynamic_body>& res
                   , asio::yield_context yield)
{
    http::async_read(con, buffer, res, yield);
}

inline
void
_recv_http_response( GenericConnection& con
                   , beast::flat_buffer& buffer
                   , http::response<http::empty_body>& res
                   , asio::yield_context yield)
{
    http::response_parser<http::empty_body> crph;
    http::async_read_header(con, buffer, crph, yield);
    res = move(crph.get());
}

template<class RequestType>
GenericConnection
maybe_perform_ssl_handshake( GenericConnection&& con
                           , const util::url_match& url
                           , RequestType req
                           , Signal<void()>& abort_signal
                           , asio::yield_context yield)
{
    using namespace std;

    //auto target = req.target().to_string();
    sys::error_code ec;

    if (url.scheme == "https") {
        auto ret = ssl::util::client_handshake( move(con)
                                              , url.host
                                              , abort_signal
                                              , yield[ec]);

        if (ec) {
            cerr << "SSL client handshake error: "
                 << url.host << ": " << ec.message() << endl;
        }

        return or_throw(yield, ec, move(ret));
    }

    return move(con);
}

template<class RequestType>
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , GenericConnection& optcon
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

    GenericConnection temp_con;

    auto& con = [&] () -> GenericConnection& {
        if (optcon.has_implementation()) {
            return optcon;
        }
        else {
            auto c = connect_to_host( ios
                                    , url.host
                                    , url.port
                                    , abort_signal
                                    , yield[ec]);

            if (ec) return temp_con;

            auto cc = maybe_perform_ssl_handshake( std::move(c)
                                                 , url
                                                 , req
                                                 , abort_signal
                                                 , yield[ec]);

            if (ec) return temp_con;

            RequestType origin_req(req);

            // Now that we have a connection to the origin we can send a
            // non-proxy request to it (i.e. with target "/foo..." and not
            // "http://example.com/foo...").  Actually some web servers do not
            // like the full form.
            auto target = req.target();
            origin_req.target(target.substr(target.find( url.path
                                                       // Length of "http://" or "https://",
                                                       // do not fail on "http(s)://FOO/FOO".
                                                       , url.scheme.length() + 3)));
            req = origin_req;

            temp_con = std::move(cc);
            return temp_con;
        }
    }();

    if (ec) return or_throw<Response>(yield, ec);

    auto ret = fetch_http<http::dynamic_body>( ios
                                             , con
                                             , req
                                             , abort_signal
                                             , yield[ec]);

    if (!ec && !optcon.has_implementation()) {
        optcon = std::move(temp_con);
    }

    return or_throw(yield, ec, std::move(ret));
}

template<class Duration, class RequestType>
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , GenericConnection& optcon
               , RequestType req
               , Duration timeout
               , Signal<void()>& abort_signal
               , asio::yield_context yield)
{
    return util::with_timeout
        ( ios
        , abort_signal
        , timeout
        , [&] (auto& abort_signal, auto yield) {
              return fetch_http_page
                (ios, optcon, req, abort_signal, yield);
          }
        , yield);
}

template<class RequestType>
http::response<http::dynamic_body>
fetch_http_origin( asio::io_service& ios
                 , GenericConnection& con_
                 , const util::url_match& url
                 , RequestType req
                 , Signal<void()>& abort_signal
                 , asio::yield_context yield)
{
    using namespace std;
    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;

    auto con = maybe_perform_ssl_handshake( move(con_)
                                          , url
                                          , req
                                          , abort_signal
                                          , yield[ec]);
    if (ec) {
        return or_throw<Response>(yield, ec);
    }

    auto target = req.target();

    // Now that we have a connection to the origin
    // we can send a non-proxy request to it
    // (i.e. with target "/foo..." and not "http://example.com/foo...").
    // Actually some web servers do not like the full form.
    RequestType origin_req(req);
    origin_req.target(target.substr(target.find( url.path
                                               // Length of "http://" or "https://",
                                               // do not fail on "http(s)://FOO/FOO".
                                               , url.scheme.length() + 3)));

    return fetch_http<http::dynamic_body>( ios
                                         , con
                                         , origin_req
                                         , abort_signal
                                         , yield);
}

template<class Duration, class RequestType>
http::response<http::dynamic_body>
fetch_http_origin( asio::io_service& ios
                 , GenericConnection& con
                 , const util::url_match& url
                 , RequestType req
                 , Duration timeout
                 , Signal<void()>& abort_signal
                 , asio::yield_context yield)
{
    return util::with_timeout
        ( ios
        , abort_signal
        , timeout
        , [&] (auto& abort_signal, auto yield) {
              return fetch_http_origin
                (ios, con, url, req, abort_signal, yield);
          }
        , yield);
}

} // namespace
