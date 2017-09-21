#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>

// This function produces an HTTP response for the given
// request. The type of the response object depends on the
// contents of the request, so the interface requires the
// caller to pass a generic lambda for receiving the response.
template<
    class Body, class Allocator,
    class Send>
void
handle_request(
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

//------------------------------------------------------------------------------

// Report a failure
void
fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

// Handles an HTTP server connection
class session : public std::enable_shared_from_this<session>
{
    tcp::socket _socket;
    boost::asio::io_service::strand _strand;
    boost::beast::flat_buffer _buffer;
    http::request<http::string_body> _req;

public:
    // Take ownership of the socket
    explicit
    session(tcp::socket socket)
        : _socket(std::move(socket))
        , _strand(_socket.get_io_service())
    {
    }

    // Start the asynchronous operation
    void
    run()
    {
        do_read();
    }

    void
    do_read()
    {
        // Read a request
        http::async_read(_socket, _buffer, _req,
            _strand.wrap([self = shared_from_this()]
                         (auto _1, auto _2) {
                             self->on_read(_1, _2);
                         }));
    }

    void
    on_read(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        // This means they closed the connection
        if(ec == http::error::end_of_stream)
            return do_close();

        if(ec)
            return fail(ec, "read");

        // Send the response
        handle_request(std::move(_req), [this](auto&& msg) {
                // The lifetime of the message has to extend
                // for the duration of the async operation so
                // we use a shared_ptr to manage it.
                auto sp = std::make_shared<std::decay_t<decltype(msg)>>(std::move(msg));

                http::async_write(
                    _socket,
                    *sp,
                    _strand.wrap([sp, self = this->shared_from_this()]
                                 (auto ec, auto transferred) {
                                     self->on_write(ec, transferred);
                                 }));
            });
    }

    void
    on_write(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

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

    void
    do_close()
    {
        // Send a TCP shutdown
        boost::system::error_code ec;
        _socket.shutdown(tcp::socket::shutdown_send, ec);

        // At this point the connection is closed gracefully
    }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
    tcp::acceptor _acceptor;
    tcp::socket _socket;

public:
    listener(
        boost::asio::io_service& ios,
        tcp::endpoint endpoint)
        : _acceptor(ios)
        , _socket(ios)
    {
        boost::system::error_code ec;

        // Open the acceptor
        _acceptor.open(endpoint.protocol(), ec);
        if(ec) {
            fail(ec, "open");
            return;
        }

        // Bind to the server address
        _acceptor.bind(endpoint, ec);
        if(ec) {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        _acceptor.listen(
            boost::asio::socket_base::max_connections, ec);

        if(ec) {
            fail(ec, "listen");
            return;
        }
    }

    // Start accepting incoming connections
    void run()
    {
        if(! _acceptor.is_open())
            return;
        do_accept();
    }

    void do_accept()
    {
        _acceptor.async_accept(
            _socket,
            [this, self = shared_from_this()](boost::system::error_code ec) {
                if (ec) {
                    fail(ec, "accept");
                    return;
                }

                std::make_shared<session>(std::move(_socket))->run();

                do_accept();
            });
    }
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 4)
    {
        std::cerr <<
            "Usage: http-server-async <address> <port> <threads>\n" <<
            "Example:\n" <<
            "    http-server-async 0.0.0.0 8080 . 1\n";
        return EXIT_FAILURE;
    }

    auto const address = boost::asio::ip::address::from_string(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));
    auto const threads = std::max<std::size_t>(1, std::atoi(argv[3]));

    // The io_service is required for all I/O
    boost::asio::io_service ios{threads};

    // Create and launch a listening port
    std::make_shared<listener>(
        ios,
        tcp::endpoint{address, port})->run();

    // Run the I/O service on the requested number of threads
    std::vector<std::thread> v;
    v.reserve(threads - 1);

    for(auto i = threads - 1; i > 0; --i)
        v.emplace_back(
        [&ios]
        {
            ios.run();
        });
    ios.run();

    return EXIT_SUCCESS;
}
