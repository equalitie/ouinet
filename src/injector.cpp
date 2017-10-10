#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <iostream>

#include <ipfs_cache/injector.h>

#include "namespaces.h"
#include "fetch_http_page.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::dynamic_body>;

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
    res.body() = "Not local";
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(socket, res, yield[ec]);
}

//------------------------------------------------------------------------------
static
void serve( tcp::socket socket
          , shared_ptr<ipfs_cache::Injector> injector
          , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(socket, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        // Fetch the content from origin
        auto res = fetch_http_page(socket.get_io_service(), req, ec, yield);
        if (ec) return fail(ec, "fetch_http_page");

        stringstream ss;
        ss << res;
        auto key = req.target().to_string();

        injector->insert_content(key , ss.str(), [key] (sys::error_code ec, auto) {
                if (!ec) {
                    cout << "*Inserted " << key << endl;
                }
                else {
                    cout << "!Inserted " << key << " " << ec.message() << endl;
                }
            });

        // Forward back the response
        http::async_write(socket, res, yield[ec]);
        if (ec) return fail(ec, "write");
    }

    socket.shutdown(tcp::socket::shutdown_send, ec);
}

//------------------------------------------------------------------------------
void start( asio::io_service& ios
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

    auto ipfs_cache_injector = make_shared<ipfs_cache::Injector>(ios, "injector_repo");

    std::cout << "IPNS DB: " << ipfs_cache_injector->ipns_id() << endl;

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , ipfs_cache_injector
                         ](asio::yield_context yield) mutable {
                             serve(move(s), ipfs_cache_injector, yield);
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
            "Usage: injector <address> <port>\n"
            "Example:\n"
            "    injector 0.0.0.0 8080\n";

        return EXIT_FAILURE;
    }

    auto const address = asio::ip::address::from_string(argv[1]);
    auto const port = static_cast<unsigned short>(atoi(argv[2]));

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {
              start(ios , tcp::endpoint{address, port}, yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
