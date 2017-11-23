#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
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

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::dynamic_body>;

static string REPO_ROOT;

//------------------------------------------------------------------------------
template<class Fields>
static bool ok_to_cache(const http::response_header<Fields>& hdr)
{
    using string_view = beast::string_view;

    auto cc_i = hdr.find(http::field::cache_control);

    if (cc_i == hdr.end()) return true;

    auto trim_whitespace = [](string_view& v) {
        while (v.starts_with(' ')) v.remove_prefix(1);
        while (v.ends_with  (' ')) v.remove_suffix(1);
    };

    auto key_val = [&trim_whitespace](string_view v) {
        auto eq = v.find('=');

        if (eq == string_view::npos) {
            trim_whitespace(v);
            return make_pair(v, string_view("", 0));
        }

        auto key = v.substr(0, eq);
        auto val = v.substr(eq + 1, v.size());

        trim_whitespace(key);
        trim_whitespace(val);

        return make_pair(key, val);
    };

    auto for_each = [&key_val] (string_view v, auto can_cache) {
        while (true) {
            auto comma = v.find(',');

            if (comma == string_view::npos) {
                if (v.size()) {
                    if (!can_cache(key_val(v))) return false;
                }
                break;
            }

            if (!can_cache(key_val(v.substr(0, comma)))) return false;
            v.remove_prefix(comma + 1);
        }

        return true;
    };

    return for_each(cc_i->value(), [] (auto kv) {
        auto key = kv.first;
        //auto val = kv.second;
        // https://developers.google.com/web/fundamentals/performance/optimizing-content-efficiency/http-caching
        if (key == "no-store")              return false;
        //if (key == "no-cache")              return false;
        //if (key == "max-age" && val == "0") return false;
        return true;
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

        if (ok_to_cache(res)) {
            stringstream ss;
            ss << res;
            auto key = req.target().to_string();

            injector.insert_content(key, ss.str(),
                [key] (sys::error_code ec, auto) {
                    if (ec) {
                        cout << "!Insert failed: " << key << " " << ec.message() << endl;
                    }
                });
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
            asio::steady_timer timer(ios);
            timer.expires_from_now(chrono::seconds(1));
            timer.async_wait(yield[ec]);
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

    gc::Service service(REPO_ROOT + "/gnunet/peer.conf", ios);

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
            // TODO: Don't return, sleep a little and then retry.
            return;
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

    ifstream ouinet_conf(REPO_ROOT + "/ouinet.conf");

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

    // The io_service is required for all I/O
    asio::io_service ios;

    ipfs_cache::Injector ipfs_cache_injector(ios, REPO_ROOT + "/ipfs");

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
