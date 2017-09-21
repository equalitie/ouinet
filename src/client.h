#pragma once

#include <boost/asio/connect.hpp>
#include "fail.h"

namespace ouinet {

// Performs an HTTP GET and prints the response
class Client : public std::enable_shared_from_this<Client>
{
public:
    using tcp = boost::asio::ip::tcp;
    using Request  = boost::beast::http::request <boost::beast::http::string_body>;
    using Response = boost::beast::http::response<boost::beast::http::string_body>;
    using Error = boost::system::error_code;
    using Handler = std::function<void(Error, Response)>;

public:
    // Resolver and socket require an io_service
    explicit
    Client(boost::asio::io_service& ios)
        : _resolver(ios)
        , _socket(ios)
    {
    }

    // Start the asynchronous operation
    void run( std::string host
            , std::string port
            , Request req
            , Handler handler)
    {
        namespace http = boost::beast::http;

        _req = std::move(req);
        _handler = std::move(handler);

        // Look up the domain name
        _resolver.async_resolve({host, port},
            [this, self = shared_from_this()] (auto ec, auto result) {
                if (ec) {
                    return this->finish(ec, "resolve");
                }

                // Connect
                boost::asio::async_connect(this->_socket, result,
                    [self = this->shared_from_this()](auto ec, auto) {
                        self->on_connect(ec);
                    });
            });
    }

private:
    void on_connect(Error ec)
    {
        namespace http = boost::beast::http;

        if(ec)
            return fail(ec, "connect");

        // Send the HTTP request to the remote host
        http::async_write(_socket, _req,
            [self = shared_from_this()](auto ec, auto bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec) return self->finish(ec, "write");
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
                if(ec) return self->finish(ec, "read");
                self->on_read();
            });
    }

    void on_read()
    {
        // Gracefully close the socket
        Error ec;
        _socket.shutdown(tcp::socket::shutdown_both, ec);

        finish(Error(), "on_read");

        // If we get here then the connection is closed gracefully
    }

    void finish(Error ec, const char* msg)
    {
        if (ec) fail(ec, msg);
        _handler(ec, _res);
    }

private:
    tcp::resolver _resolver;
    tcp::socket _socket;
    boost::beast::flat_buffer _buffer;
    Request _req;
    Response _res;
    Handler _handler;
};

} // ouinet namespace
