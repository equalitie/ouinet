#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "fail.h"
#include "connect_to_host.h"
#include "or_throw.h"

namespace ouinet {

template<class RequestType>
inline
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , GenericConnection& con
               , RequestType req
               , asio::yield_context yield)
{
    http::response<http::dynamic_body> res;

    sys::error_code ec;

    // Send the HTTP request to the remote host
    http::async_write(con, req, yield[ec]);
    if (ec) return or_throw(yield, ec, move(res));

    beast::flat_buffer buffer;

    // Receive the HTTP response
    http::async_read(con, buffer, res, yield[ec]);

    return or_throw(yield, ec, move(res));
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

    return fetch_http_page(ios, con, std::move(req), yield[ec]);
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
