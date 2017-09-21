#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <memory>

namespace ouinet {

class ProxySession : public std::enable_shared_from_this<ProxySession>
{
    using tcp = boost::asio::ip::tcp;

public:
    // Take ownership of the socket
    explicit
    ProxySession(tcp::socket socket)
        : _socket(std::move(socket))
    { }

    // Start the asynchronous operation
    void run() { do_read(); }

private:
    void do_read();
    void do_close();

    void on_read(boost::system::error_code);
    void on_write(boost::system::error_code);

    template<class Body, class Allocator, class Send>
    void handle_request(boost::beast::http::request
                            < Body
                            , boost::beast::http::basic_fields<Allocator>>&& req
                            , Send&& send);

private:
    tcp::socket _socket;
    boost::beast::flat_buffer _buffer;
    boost::beast::http::request<boost::beast::http::string_body> _req;
};

} // ouinet namespace
