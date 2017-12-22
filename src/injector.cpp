#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <fstream>

#include <ipfs_cache/injector.h>
#include <gnunet_channels/channel.h>
#include <gnunet_channels/cadet_port.h>
#include <gnunet_channels/service.h>

#include "namespaces.h"
#include "util.h"
#include "fetch_http_page.h"
#include "generic_connection.h"
#include "split_string.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"

using namespace std;
using namespace ouinet;

namespace fs = boost::filesystem;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::dynamic_body>;

static fs::path REPO_ROOT;
static const fs::path OUINET_CONF_FILE = "ouinet-injector.conf";

//------------------------------------------------------------------------------
// Cache control:
// https://tools.ietf.org/html/rfc7234
// https://tools.ietf.org/html/rfc5861
// https://tools.ietf.org/html/rfc8246
//
// For a less dry reading:
// https://developers.google.com/web/fundamentals/performance/optimizing-content-efficiency/http-caching
//
// TODO: This function is incomplete.
static bool ok_to_cache( const http::request_header<http::fields>&  request
                       , const http::response_header<http::fields>& response)
{
    auto cache_control_i = response.find(http::field::cache_control);

    // https://tools.ietf.org/html/rfc7234#section-3 (bullet #5)
    if (request.count(http::field::authorization)) {
        // https://tools.ietf.org/html/rfc7234#section-3.2
        if (cache_control_i == response.end()) return false;

        for (auto v : SplitString(cache_control_i->value(), ',')) {
            if (v == "must-revalidate") return true;
            if (v == "public")          return true;
            if (v == "s-maxage")        return true;
        }
    }

    if (cache_control_i == response.end()) return true;

    for (auto kv : SplitString(cache_control_i->value(), ','))
    {
        beast::string_view key, val;
        std::tie(key, val) = split_string_pair(kv, '=');

        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
        if (key == "no-store") return false;
        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #4)
        if (key == "private")  return false;
    }

    return true;
}

//------------------------------------------------------------------------------
static
void try_to_cache( ipfs_cache::Injector& injector
                 , const http::request_header<http::fields>& request
                 , const http::response<http::dynamic_body>& response)
{
    if (!ok_to_cache(request, response)) {
        return;
    }

    stringstream ss;
    ss << response;
    auto key = request.target().to_string();

    injector.insert_content(key, ss.str(),
        [key] (sys::error_code ec, auto) {
            if (ec) {
                cout << "!Insert failed: " << key << " " << ec.message() << endl;
            }
        });
}

//------------------------------------------------------------------------------
static
void serve( shared_ptr<GenericConnection> con
          , ipfs_cache::Injector& injector
          , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(*con, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        // Fetch the content from origin
        auto res = fetch_http_page(con->get_io_service(), req, ec, yield);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "fetch_http_page");

        switch (res.result()) {
            case http::status::ok:
            case http::status::moved_permanently:
                try_to_cache(injector, req, res);
                break;
            // TODO: Other response codes
            default:
                cerr << "Warning: not handling: " << res.result()
                     << " HTTP response in terms of cache" << endl;
        }

        // Forward back the response
        http::async_write(*con, res, yield[ec]);
        if (ec) return fail(ec, "write");
    }
}

//------------------------------------------------------------------------------
static
void listen_tcp( asio::io_service& ios
               , tcp::endpoint endpoint
               , ipfs_cache::Injector& ipfs_cache_injector
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
            // Wait one second before we start accepting again.
            async_sleep(ios, chrono::seconds(1), yield);
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , &ipfs_cache_injector
                         ](asio::yield_context yield) mutable {
                             using Con = GenericConnectionImpl<tcp::socket>;

                             auto con = make_shared<Con>(move(s));
                             serve(con, ipfs_cache_injector, yield);

                             sys::error_code ec;
                             con->get_impl().shutdown(tcp::socket::shutdown_send, ec);
                         });
        }
    }
}

//------------------------------------------------------------------------------
static
void listen_gnunet( asio::io_service& ios
                  , string port_str
                  , ipfs_cache::Injector& ipfs_cache_injector
                  , asio::yield_context yield)
{
    namespace gc = gnunet_channels;

    gc::Service service((REPO_ROOT/"gnunet"/"peer.conf").native(), ios);

    sys::error_code ec;

    service.async_setup(yield[ec]);

    if (ec) {
        cerr << "Failed to setup GNUnet service: " << ec.message() << endl;
        return;
    }

    cout << "GNUnet ID: " << service.identity() << endl;

    gc::CadetPort port(service);

    while (true) {
        gc::Channel channel(service);
        port.open(channel, port_str, yield[ec]);

        if (ec) {
            cerr << "Failed to accept: " << ec.message() << endl;
            async_sleep(ios, chrono::milliseconds(100), yield);
            continue;
        }

        asio::spawn( ios
                   , [ channel = move(channel)
                     , &ipfs_cache_injector
                     ](auto yield) mutable {
                        using Con = GenericConnectionImpl<gnunet_channels::Channel>;

                        serve( make_shared<Con>(move(channel))
                             , ipfs_cache_injector
                             , yield);
                     });
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
        ("listen-on-gnunet", po::value<string>(), "GNUnet port on which we'll listen")
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << endl;
        return 0;
    }

    if (!vm.count("repo")) {
        cerr << "The 'repo' argument is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    REPO_ROOT = vm["repo"].as<string>();

    if (!exists(REPO_ROOT) || !is_directory(REPO_ROOT)) {
        cerr << "The path " << REPO_ROOT << " either doesn't exist or"
             << " is not a directory." << endl;
        cerr << desc << endl;
        return 1;
    }

    fs::path ouinet_conf_path = REPO_ROOT/OUINET_CONF_FILE;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        cerr << "The path " << REPO_ROOT << " does not contain the "
             << OUINET_CONF_FILE << " configuration file." << endl;
        cerr << desc << endl;
        return 1;
    }

    ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (!vm.count("listen-on-tcp") && !vm.count("listen-on-gnunet")) {
        cerr << "Either 'listen-on-tcp' or 'listen-on-gnunet' (or both)"
             << " arguments must be specified" << endl;
        cerr << desc << endl;
        return 1;
    }

    auto const injector_ep
        = util::parse_endpoint(vm["listen-on-tcp"].as<string>());

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    // The io_service is required for all I/O
    asio::io_service ios;

    ipfs_cache::Injector ipfs_cache_injector(ios, (REPO_ROOT/"ipfs").native());

    std::cout << "IPNS DB: " << ipfs_cache_injector.ipns_id() << endl;

    if (vm.count("listen-on-tcp")) {
        asio::spawn
            ( ios
            , [&](asio::yield_context yield) {
                  listen_tcp(ios, injector_ep, ipfs_cache_injector, yield);
              });
    }

    if (vm.count("listen-on-gnunet")) {
        asio::spawn
            ( ios
            , [&](asio::yield_context yield) {
                  string port = vm["listen-on-gnunet"].as<string>();
                  listen_gnunet(ios, port, ipfs_cache_injector, yield);
              });
    }

    ios.run();

    return EXIT_SUCCESS;
}
