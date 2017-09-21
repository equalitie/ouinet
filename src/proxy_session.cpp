#include "proxy_session.h"
#include "client.h"
#include "fail.h"

using namespace ouinet;

namespace http = boost::beast::http;

void ProxySession::do_read()
{
    // Read a request
    http::async_read(_socket, _buffer, _req,
        [self = shared_from_this()] (auto ec, auto) {
            self->on_read(ec);
        });
}

void ProxySession::on_read(boost::system::error_code ec)
{
    // This means they closed the connection
    if(ec == http::error::end_of_stream)
        return do_close();

    if(ec)
        return fail(ec, "read");

    std::cout << "Request: " << _req << std::endl;
    // Send the response
    handle_request(std::move(_req), [this](auto&& msg) {
            // The lifetime of the message has to extend
            // for the duration of the async operation so
            // we use a shared_ptr to manage it.
            auto sp = std::make_shared<std::decay_t<decltype(msg)>>(std::move(msg));

            http::async_write(
                _socket,
                *sp,
                [sp, self = this->shared_from_this()]
                (auto ec, auto bytes_transferred) {
                    boost::ignore_unused(bytes_transferred);
                    self->on_write(ec);
                });
        });
}

void ProxySession::on_write(boost::system::error_code ec)
{
    if(ec == http::error::end_of_stream) {
        // This means we should close the connection, usually because
        // the response indicated the "Connection: close" semantic.
        return do_close();
    }

    if(ec)
        return fail(ec, "write");

    // Read another request
    do_read();
}

template<
    class Body, class Allocator,
    class Send>
void ProxySession::handle_request(
    http::request<Body, http::basic_fields<Allocator>>&& req,
    Send&& send)
{
    // Make sure we can handle the method
    if( req.method() != http::verb::get && req.method() != http::verb::head) {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "Unknown HTTP-method";
        res.prepare_payload();
        send(res);
        return;
    }

    auto mime_type = "text/plain";
    std::string body = "Client requested " + req.target().to_string() + "\n";

    // Respond to HEAD request
    if(req.method() == http::verb::head) {
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type);
        res.content_length(body.size());
        res.keep_alive(req.keep_alive());
        return send(std::move(res));
    }

    // Respond to GET request
    http::response<http::string_body> res{
        std::piecewise_construct,
        std::make_tuple(body),
        std::make_tuple(http::status::ok, req.version())};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, mime_type);
    res.content_length(body.size());
    res.keep_alive(req.keep_alive());

    return send(std::move(res));
}

void ProxySession::do_close()
{
    // Send a TCP shutdown
    boost::system::error_code ec;
    _socket.shutdown(tcp::socket::shutdown_send, ec);

    // At this point the connection is closed gracefully
}
