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
               , const std::string& host
               , RequestType req
               , sys::error_code& ec
               , asio::yield_context yield)
{
    using tcp = asio::ip::tcp;

    http::response<http::dynamic_body> res;

    auto finish = [&res](auto ec, auto what) {
        fail(ec, what);
        return res;
    };

    tcp::socket socket = connect_to_host(ios, host, ec, yield);
    if (ec) return finish(ec, "fetch_http_page::resolve");

    // Send the HTTP request to the remote host
    http::async_write(socket, req, yield[ec]);
    if (ec) return finish(ec, "fetch_http_page::write");

    beast::flat_buffer buffer;

    // Receive the HTTP response
    http::async_read(socket, buffer, res, yield[ec]);

    if (ec == asio::error::connection_reset) return res;
    if (ec == http::error::end_of_stream)    return res;

    if (ec) return finish(ec, "fetch_http_page::read");

    // Gracefully close the socket
    socket.shutdown(tcp::socket::shutdown_both, ec);

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
               , RequestType req
               , sys::error_code& ec
               , asio::yield_context yield)
{
    auto host = req["host"].to_string();
    return fetch_http_page(ios, host, std::move(req), ec, yield);
}

}
