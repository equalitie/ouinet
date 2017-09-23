#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <iostream>

using namespace std;

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace sys   = boost::system;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;

//------------------------------------------------------------------------------
// Report a failure
static
void fail(sys::error_code ec, char const* what)
{
    cerr << what << ": " << ec.message() << "\n";
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
                   , sys::error_code& ec
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

    tcp::resolver resolver{ios};

    // Look up the domain name
    auto const lookup = resolver.async_resolve({host, port}, yield[ec]);
    if (ec) return finish(ec, "resolve");

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return finish(ec, "connect");

    return socket;
}

//------------------------------------------------------------------------------
static
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , Request req
               , sys::error_code& ec
               , asio::yield_context yield)
{
    http::response<http::dynamic_body> res;

    auto finish = [&res](auto ec, auto what) {
        fail(ec, what);
        return res;
    };

    tcp::socket socket = connect(ios, req["host"], ec, yield);
    if (ec) return finish(ec, "resolve");

    // Send the HTTP request to the remote host
    http::async_write(socket, req, yield[ec]);
    if (ec) return finish(ec, "write");

    beast::flat_buffer buffer;

    // Receive the HTTP response
    http::async_read(socket, buffer, res, yield[ec]);
    if (ec) return finish(ec, "read");

    // Gracefully close the socket
    socket.shutdown(tcp::socket::shutdown_both, ec);

    // not_connected happens sometimes
    // so don't bother reporting it.
    if(ec && ec != sys::errc::not_connected)
        return finish(ec, "shutdown");

    return res;
}

//------------------------------------------------------------------------------
static
void handle_bad_request( tcp::socket& socket
                       , const Request& req
                       , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = "Unknown HTTP-method";
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(socket, res, yield[ec]);
}

//------------------------------------------------------------------------------
static
void forward(tcp::socket& in, tcp::socket& out, asio::yield_context yield)
{
    sys::error_code ec;
    array<uint8_t, 2048> data;

    for (;;) {
        size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
        if (ec) break;

        asio::async_write(out, asio::buffer(data, length), yield[ec]);
        if (ec) break;
    }
}

//------------------------------------------------------------------------------
static
void handle_connect_request( tcp::socket& client_s
                           , const Request& req
                           , asio::yield_context yield)
{
    sys::error_code ec;
    asio::io_service& ios = client_s.get_io_service();

    tcp::socket origin_s = connect(ios, req["host"], ec, yield);
    if (ec) return fail(ec, "connect");

    http::response<http::empty_body> res{http::status::ok, req.version()};

    // Send the client an OK message indicating that the tunnel
    // has been established. TODO: Reply with an error otherwise.
    http::async_write(client_s, res, yield[ec]);
    if (ec) return fail(ec, "sending connect response");

    struct State {
        tcp::socket client_s, origin_s;
    };

    auto s = make_shared<State>(State{move(client_s), move(origin_s)});

    asio::spawn
        ( yield
        , [s](asio::yield_context yield) {
              forward(s->client_s, s->origin_s, yield);
          });

    asio::spawn
        ( yield
        , [s](asio::yield_context yield) {
              forward(s->origin_s, s->client_s, yield);
          });
}

//------------------------------------------------------------------------------
static
void start_http_forwarding(tcp::socket socket, asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(socket, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        if (req.method() == http::verb::connect) {
            return handle_connect_request(socket, req, yield);
        }

        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            return handle_bad_request(socket, req, yield);
        }

        // Forward the request
        auto res = fetch_http_page(socket.get_io_service(), req, ec, yield);
        if (ec) return fail(ec, "fetch_http_page");

        // Forward back the response
        http::async_write(socket, res, yield[ec]);
        if (ec) return fail(ec, "write");
    }

    socket.shutdown(tcp::socket::shutdown_send, ec);
}

//------------------------------------------------------------------------------
void do_listen( asio::io_service& ios
              , tcp::endpoint endpoint
              , asio::yield_context yield)
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ios);

    acceptor.open(endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
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
            "Usage: http-server-async <address> <port>\n"
            "Example:\n"
            "    http-server-async 0.0.0.0 8080\n";

        return EXIT_FAILURE;
    }

    auto const address = asio::ip::address::from_string(argv[1]);
    auto const port = static_cast<unsigned short>(atoi(argv[2]));

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {
              do_listen( ios
                       , tcp::endpoint{address, port}
                       , yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
