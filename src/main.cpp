#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/config.hpp>
#include <iostream>

using namespace std;

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;

using tcp         = boost::asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;

//------------------------------------------------------------------------------
// Report a failure
static
void fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

//------------------------------------------------------------------------------
static
pair<string_view, string_view> split_host_port(const string_view& hp)
{
    auto pos = hp.find(':');

    if (pos == string::npos) {
        return make_pair(hp, "80");
    }

    return make_pair(hp.substr(0, pos), hp.substr(pos+1));
}

//------------------------------------------------------------------------------
static
tcp::socket connect( asio::io_service& ios
                   , string_view host_and_port
                   , boost::system::error_code& ec
                   , asio::yield_context yield)
{
    auto hp = split_host_port(host_and_port);

    string host = hp.first .to_string();
    string port = hp.second.to_string();

    tcp::socket socket(ios);

    auto finish = [&socket] (auto ec, auto where) {
        fail(ec, where);
        return move(socket);
    };

    // These objects perform our I/O
    tcp::resolver resolver{ios};

    // Look up the domain name
    auto const lookup = resolver.async_resolve({host, port}, yield[ec]);
    if (ec) return finish(ec, "resolve");

    // Make the connection on the IP address we get from a lookup
    boost::asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return finish(ec, "connect");

    return socket;
}

//------------------------------------------------------------------------------
static
http::response<http::dynamic_body>
fetch_http_page( boost::asio::io_service& ios
               , string_view host
               , Request req
               , boost::system::error_code& ec
               , boost::asio::yield_context yield)
{
    http::response<http::dynamic_body> res;

    auto finish = [&res](auto ec, auto what) {
        fail(ec, what);
        return res;
    };

    tcp::socket socket = connect(ios, host, ec, yield);
    if (ec) return finish(ec, "resolve");

    // Send the HTTP request to the remote host
    http::async_write(socket, req, yield[ec]);
    if (ec) return finish(ec, "write");

    // This buffer is used for reading and must be persisted
    boost::beast::flat_buffer b;

    // Declare a container to hold the response

    // Receive the HTTP response
    http::async_read(socket, b, res, yield[ec]);
    if (ec) return finish(ec, "read");

    // Gracefully close the socket
    socket.shutdown(tcp::socket::shutdown_both, ec);

    // not_connected happens sometimes
    // so don't bother reporting it.
    //
    if(ec && ec != boost::system::errc::not_connected)
        return finish(ec, "shutdown");

    // If we get here then the connection is closed gracefully
    
    return res;
}

//------------------------------------------------------------------------------
template<class Req>
static
void handle_bad_request( tcp::socket& socket
                       , const Req& req
                       , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = "Unknown HTTP-method";
    res.prepare_payload();

    boost::system::error_code ec;
    http::async_write(socket, res, yield[ec]);
}

//------------------------------------------------------------------------------
static
void start_http_forwarding(tcp::socket socket, boost::asio::yield_context yield)
{
    boost::system::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(socket, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream)
            break;

        if (ec) return fail(ec, "read");

        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            return handle_bad_request(socket, req, yield);
        }

        // Forward the request
        auto res = fetch_http_page( socket.get_io_service()
                                  , req["host"]
                                  , req
                                  , ec
                                  , yield);
            
        if (ec) return fail(ec, "fetch_http_page");

        http::async_write(socket, res, yield[ec]);
        if (ec) return fail(ec, "write");
    }

    socket.shutdown(tcp::socket::shutdown_send, ec);
}

//------------------------------------------------------------------------------
void do_listen( boost::asio::io_service& ios
              , tcp::endpoint endpoint
              , asio::yield_context yield)
{
    boost::system::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ios);

    acceptor.open(endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(boost::asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
        }
        else {
            asio::spawn( ios
                       , [s = move(socket)](asio::yield_context yield) mutable {
                             start_http_forwarding(move(s), yield);
                         });
        }
    }
}

//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 3)
    {
        cerr <<
            "Usage: http-server-async <address> <port>\n" <<
            "Example:\n" <<
            "    http-server-async 0.0.0.0 8080\n";
        return EXIT_FAILURE;
    }

    auto const address = boost::asio::ip::address::from_string(argv[1]);
    auto const port = static_cast<unsigned short>(atoi(argv[2]));

    // The io_service is required for all I/O
    boost::asio::io_service ios;

    boost::asio::spawn
        ( ios
        , [&](boost::asio::yield_context yield) {
              do_listen( ios
                       , tcp::endpoint{address, port}
                       , yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
