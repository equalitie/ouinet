#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "fail.h"
#include "connect_to_host.h"

namespace ouinet {

template<class RequestType>
inline
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , GenericConnection& con
               , RequestType req
               , sys::error_code& ec
               , asio::yield_context yield)
{
    http::response<http::dynamic_body> res;

    auto finish = [&res](auto ec, auto what) {
        fail(ec, what);
        return res;
    };

    // Send the HTTP request to the remote host
    http::async_write(con, req, yield[ec]);
    if (ec) return finish(ec, "fetch_http_page::write");

    beast::flat_buffer buffer;

    // Receive the HTTP response
    http::async_read(con, buffer, res, yield[ec]);

    if (ec == asio::error::connection_reset) return res;
    if (ec == http::error::end_of_stream)    return res;

    if (ec) return finish(ec, "fetch_http_page::read");

    // not_connected happens sometimes
    // so don't bother reporting it.
    if(ec && ec != sys::errc::not_connected)
        return finish(ec, "fetch_http_page::shutdown");

    return res;
}

template<class RequestType>
inline
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , const std::string& host
               , RequestType req
               , sys::error_code& ec
               , asio::yield_context yield)
{
    auto con = connect_to_host(ios, host, ec, yield);

    if (ec) {
        fail(ec, "fetch_http_page::connect");
        return http::response<http::dynamic_body>();
    }

    return fetch_http_page(ios, con, std::move(req), ec, yield);
}

template<class RequestType>
inline
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , RequestType req
               , sys::error_code& ec
               , asio::yield_context yield)
{
    auto host = req["host"].to_string();
    return fetch_http_page(ios, host, std::move(req), ec, yield);
}

}
