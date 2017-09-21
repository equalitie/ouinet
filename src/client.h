#pragma once

#include <boost/asio/connect.hpp>
#include "fail.h"

namespace ouinet {

// Performs an HTTP GET and prints the response
class Client : public std::enable_shared_from_this<Client>
{
    using tcp = boost::asio::ip::tcp;

public:
    // Resolver and socket require an io_service
    explicit
    Client(boost::asio::io_service& ios)
        : _resolver(ios)
        , _socket(ios)
    {
    }

    // Start the asynchronous operation
    void run( char const* host
            , char const* port
            , char const* target)
    {
        namespace http = boost::beast::http;

        // Set up an HTTP GET request message
        _req.version(11);
        _req.method(http::verb::get);
        _req.target(target);
        _req.set(http::field::host, host);
        _req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Look up the domain name
        _resolver.async_resolve({host, port},
            [this, self = shared_from_this()] (auto ec, auto result) {
                if (ec) {
                    return fail(ec, "resolve");
                }

                // Connect
                boost::asio::async_connect(this->_socket, result,
                    [self = this->shared_from_this()](auto ec, auto) {
                        self->on_connect(ec);
                    });
            });
    }

private:
    void on_connect(boost::system::error_code ec)
    {
        namespace http = boost::beast::http;

        if(ec)
            return fail(ec, "connect");

        // Send the HTTP request to the remote host
        http::async_write(_socket, _req,
            [self = shared_from_this()](auto ec, auto bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec) return fail(ec, "write");
                self->on_write();
            });
    }

    void on_write()
    {
        namespace http = boost::beast::http;

        // Receive the HTTP response
        http::async_read(_socket, _buffer, _res,
            [self = shared_from_this()](auto ec, auto bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if(ec) return fail(ec, "read");
                self->on_read();
            });
    }

    void on_read()
    {
        // Write the message to standard out
        std::cout << _res << std::endl;

        // Gracefully close the socket
        boost::system::error_code ec;
        _socket.shutdown(tcp::socket::shutdown_both, ec);

        // not_connected happens sometimes so don't bother reporting it.
        if(ec && ec != boost::system::errc::not_connected)
            return fail(ec, "shutdown");

        // If we get here then the connection is closed gracefully
    }

private:
    tcp::resolver _resolver;
    tcp::socket _socket;
    boost::beast::flat_buffer _buffer;
    boost::beast::http::request<boost::beast::http::dynamic_body> _req;
    boost::beast::http::response<boost::beast::http::string_body> _res;
};

} // ouinet namespace
