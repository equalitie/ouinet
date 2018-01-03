#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <fstream>

#include <ipfs_cache/client.h>
#include <ipfs_cache/error.h>

#include <gnunet_channels/channel.h>
#include <gnunet_channels/service.h>

#include "namespaces.h"
#include "connect_to_host.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_connection.h"
#include "util.h"
#include "result.h"
#include "blocker.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "endpoint.h"

using namespace std;
using namespace ouinet;

namespace fs = boost::filesystem;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;

static fs::path REPO_ROOT;
static const fs::path OUINET_CONF_FILE = "ouinet-client.conf";

//------------------------------------------------------------------------------
struct Client {
    asio::io_service& ios;
    unique_ptr<gnunet_channels::Service> gnunet_service;

    Client(asio::io_service& ios) : ios(ios) {}
};

//------------------------------------------------------------------------------
static
void handle_bad_request( GenericConnection& con
                       , const Request& req
                       , string message
                       , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = message;
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(con, res, yield[ec]);
}

//------------------------------------------------------------------------------
static
void forward(GenericConnection& in, GenericConnection& out, asio::yield_context yield)
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
void handle_connect_request( GenericConnection& client_c
                           , const Request& req
                           , asio::yield_context yield)
{
    sys::error_code ec;
    asio::io_service& ios = client_c.get_io_service();

    auto origin_c = connect_to_host(ios, req["host"], ec, yield);
    if (ec) return fail(ec, "connect");

    // Send the client an OK message indicating that the tunnel
    // has been established. TODO: Reply with an error otherwise.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    http::async_write(client_c, res, yield[ec]);

    if (ec) return fail(ec, "sending connect response");

    Blocker blocker(ios);

    asio::spawn
        ( yield
        , [&, b = blocker.make_block()](asio::yield_context yield) {
              forward(client_c, *origin_c, yield);
          });

    asio::spawn
        ( yield
        , [&, b = blocker.make_block()](asio::yield_context yield) {
              forward(*origin_c, client_c, yield);
          });

    blocker.wait(yield);
}

//------------------------------------------------------------------------------
static bool is_front_end_request(const Request& req)
{
    auto host = req["Host"].to_string();

    if (host.substr(0, sizeof("localhost")) != "localhost") {
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
static
Result<unique_ptr<GenericConnection>>
connect_to_injector( Endpoint endpoint
                   , Client& client
                   , asio::yield_context yield)
{
    struct Visitor {
        using Ret = Result<unique_ptr<GenericConnection>>;

        sys::error_code ec;
        Client& client;
        asio::yield_context yield;

        Visitor(Client& client, asio::yield_context yield)
            : client(client), yield(yield) {}

        Ret operator()(const tcp::endpoint& ep) {
            tcp::socket socket(client.ios);
            socket.async_connect(ep, yield[ec]);

            if (ec) return ec;
            return make_unique<GenericConnectionImpl<tcp::socket>>(move(socket));
        }

        Ret operator()(const GnunetEndpoint& ep) {
            using Channel = gnunet_channels::Channel;

            if (!client.gnunet_service) {
                return asio::error::no_protocol_option;
            }

            Channel ch(*client.gnunet_service);
            ch.connect(ep.host, ep.port, yield[ec]);

            if (ec) return ec;
            return make_unique<GenericConnectionImpl<Channel>>(move(ch));
        }
    };

    Visitor visitor(client, yield);

    return boost::apply_visitor(visitor, endpoint);
}

//------------------------------------------------------------------------------
static void serve_request( shared_ptr<GenericConnection> con
                         , Endpoint injector_ep
                         , shared_ptr<ipfs_cache::Client> cache_client
                         , shared_ptr<ClientFrontEnd> front_end
                         , Client& client
                         , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(*con, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        if (req.method() == http::verb::connect) {
            return handle_connect_request(*con, req, yield);
        }

        if (is_front_end_request(req)) {
            return front_end->serve(*con, req, cache_client, yield);
        }

        // TODO: We're not handling HEAD requests correctly.
        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            return handle_bad_request(*con, req, "Bad request", yield);
        }

        if (cache_client && front_end->is_ipfs_cache_enabled()) {
            // Get the content from cache
            auto key = req.target();

            string content = cache_client->get_content(key.to_string(), yield[ec]);

            if (!ec) {
                asio::async_write(*con, asio::buffer(content), yield[ec]);
                if (ec) return fail(ec, "async_write");
                continue;
            }

            if (ec != ipfs_cache::error::key_not_found) {
                cout << "Failed to fetch from DB " << ec.message()
                     << " " << req.target() << endl;
            }
        }

        if (!front_end->is_injector_proxying_enabled()) {
            return handle_bad_request(*con , req , "Not cached" , yield);
        }

        auto inj_con = connect_to_injector(injector_ep, client, yield);

        if (inj_con.is_error()) return fail(inj_con.get_error(), "channel connect");

        // Forward the request to the injector
        auto res = fetch_http_page(con->get_io_service(), **inj_con, req, ec, yield);
        if (ec) return fail(ec, "fetch_http_page");

        // Forward back the response
        http::async_write(*con, res, yield[ec]);
        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "write");
    }
}

//------------------------------------------------------------------------------
void do_listen( Client& client
              , tcp::endpoint local_endpoint
              , Endpoint injector
              , string ipns
              , asio::yield_context yield)
{
    auto& ios = client.ios;

    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ios);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

    shared_ptr<ipfs_cache::Client> ipfs_cache_client;

    if (ipns.size()) {
        ipfs_cache_client = make_shared<ipfs_cache::Client>
            (ios, ipns, (REPO_ROOT/"ipfs").native());
    }

    cout << "Client accepting on " << acceptor.local_endpoint() << endl;

    auto front_end = make_shared<ClientFrontEnd>();

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
            async_sleep(ios, chrono::seconds(1), yield);
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , ipfs_cache_client
                         , injector
                         , front_end
                         , &client
                         ](asio::yield_context yield) mutable {
                             using Con = GenericConnectionImpl<tcp::socket>;
                             auto con = make_shared<Con>(move(s));

                             serve_request( con
                                          , std::move(injector)
                                          , move(ipfs_cache_client)
                                          , move(front_end)
                                          , client
                                          , yield);

                             sys::error_code ec;
                             con->get_impl().shutdown(tcp::socket::shutdown_send, ec);
                         });
        }
    }
}

//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    namespace po = boost::program_options;

    po::options_description desc("\nOptions");

    desc.add_options()
        ("help", "Produce this help message")
        ("repo", po::value<string>(), "Path to the repository root")
        ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen")
        ("injector-ep"
         , po::value<string>()
         , "Injector's endpoint (either <IP>:<PORT> or <GNUnet's ID>:<GNUnet's PORT>")
        ("injector-ipns"
         , po::value<string>()->default_value("")
         , "IPNS of the injector's database")
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (!vm.count("repo")) {
        cerr << "The 'repo' argument is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    REPO_ROOT = fs::path(vm["repo"].as<string>());

    if (!fs::exists(REPO_ROOT)) {
        cerr << "Directory " << REPO_ROOT << " does not exist." << endl;
        cerr << desc << endl;
        return 1;
    }

    if (!fs::is_directory(REPO_ROOT)) {
        cerr << "The path " << REPO_ROOT << " is not a directory." << endl;
        cerr << desc << endl;
        return 1;
    }

    fs::path ouinet_conf_path = REPO_ROOT/OUINET_CONF_FILE;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        cerr << "The path " << REPO_ROOT << " does not contain "
             << "the " << OUINET_CONF_FILE << " configuration file." << endl;
        cerr << desc << endl;
        return 1;
    }

    ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    if (!vm.count("listen-on-tcp")) {
        cerr << "The parameter 'listen-on-tcp' is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    if (!vm.count("injector-ep")) {
        cerr << "The parameter 'injector-ep' is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    auto const local_ep = util::parse_endpoint(vm["listen-on-tcp"].as<string>());
    auto const injector_ep = *parse_endpoint(vm["injector-ep"].as<string>());

    string ipns;

    if (vm.count("injector-ipns")) {
        ipns = vm["injector-ipns"].as<string>();
    }

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {

              Client client(ios);

              if (is_gnunet_endpoint(injector_ep)) {
                  namespace gc = gnunet_channels;

                  string config = (REPO_ROOT/"gnunet"/"peer.conf").native();

                  auto service = make_unique<gc::Service>(config, ios);

                  sys::error_code ec;

                  cout << "Setting up GNUnet ..." << endl;
                  service->async_setup(yield[ec]);

                  if (ec) {
                      cerr << "Failed to setup GNUnet service: "
                           << ec.message() << endl;
                      return;
                  }

                  cout << "GNUnet ID: " << service->identity() << endl;

                  client.gnunet_service = move(service);
              }

              do_listen( client
                       , local_ep
                       , injector_ep
                       , ipns
                       , yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
