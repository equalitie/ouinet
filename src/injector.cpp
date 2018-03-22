#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <fstream>
#include <string>

#include <ipfs_cache/injector.h>
//#include <i2poui.h>

#include "namespaces.h"
#include "util.h"
#include "fetch_http_page.h"
#include "connect_to_host.h"
#include "cache_control.h"
#include "generic_connection.h"
#include "split_string.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "full_duplex_forward.h"

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"

#include "util/signal.h"

using namespace std;
using namespace ouinet;

namespace fs = boost::filesystem;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;
using Response    = http::response<http::dynamic_body>;

static fs::path REPO_ROOT;
static const fs::path OUINET_CONF_FILE = "ouinet-injector.conf";
static const fs::path OUINET_PID_FILE = "pid";

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
void handle_connect_request( GenericConnection& client_c
                           , const Request& req
                           , Signal<void()>& disconnect_signal
                           , asio::yield_context yield)
{
    sys::error_code ec;

    asio::io_service& ios = client_c.get_io_service();

    auto disconnect_client_slot = disconnect_signal.connect([&client_c] {
        client_c.close();
    });

    auto origin_c = connect_to_host(ios, req["host"], disconnect_signal, yield[ec]);

    if (ec) {
        return handle_bad_request( client_c
                                 , req
                                 , "Connect: can't connect to the origin"
                                 , yield[ec]);
    }

    auto disconnect_origin_slot = disconnect_signal.connect([&origin_c] {
        origin_c.close();
    });

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.prepare_payload();

    http::async_write(client_c, res, yield[ec]);

    if (ec) return fail(ec, "sending connect response");

    full_duplex(client_c, origin_c, yield);
}

//------------------------------------------------------------------------------
struct InjectorCacheControl {
public:
    // TODO: Replace this with cancellation support in which fetch_ operations get a signal parameter
    InjectorCacheControl( asio::io_service& ios
                        , unique_ptr<ipfs_cache::Injector>& injector
                        , Signal<void()>& abort_signal)
        : ios(ios)
        , injector(injector)
    {
        cc.fetch_fresh = [this, &ios, &abort_signal](const Request& rq, asio::yield_context yield) {
            auto host = rq["host"].to_string();

            sys::error_code ec;
            auto con = connect_to_host(ios, host, abort_signal, yield[ec]);
            if (ec) return or_throw<Response>(yield, ec);

            auto close_con_slot = abort_signal.connect([&con] {
                con.close();
            });
            return fetch_http_page(ios, con, rq, yield);
        };

        cc.fetch_stored = [this](const Request& rq, asio::yield_context yield) {
            return this->fetch_stored(rq, yield);
        };

        cc.store = [this](const Request& rq, const Response& rs) {
            this->insert_content(rq, rs);
        };
    }

    Response fetch(const Request& rq, asio::yield_context yield)
    {
        return cc.fetch(rq, yield);
    }

private:
    void insert_content(const Request& rq, const Response& rs)
    {
        if (!injector) return;

        stringstream ss;
        ss << rs;
        auto key = rq.target().to_string();

        injector->insert_content(key, ss.str(),
            [key] (const sys::error_code& ec, auto) {
                if (ec) {
                    cout << "!Insert failed: " << key
                         << " " << ec.message() << endl;
                }
            });
    }

    CacheControl::CacheEntry
    fetch_stored(const Request& rq, asio::yield_context yield)
    {
        using CacheEntry = CacheControl::CacheEntry;

        if (!injector)
            return or_throw<CacheEntry>( yield
                                       , asio::error::operation_not_supported);

        sys::error_code ec;

        auto content = injector->get_content(rq.target().to_string(), yield[ec]);

        if (ec) return or_throw<CacheEntry>(yield, ec);

        http::response_parser<Response::body_type> parser;
        parser.eager(true);
        parser.put(asio::buffer(content.data), ec);
        assert(!ec && "Malformed cache entry");

        if (!parser.is_done()) {
            cerr << "------- WARNING: Unfinished message in cache --------" << endl;
            assert(parser.is_header_done() && "Malformed response head did not cause error");
            auto rp = parser.get();
            cerr << rq << rp.base() << "<" << rp.body().size() << " bytes in body>" << endl;
            cerr << "-----------------------------------------------------" << endl;
            ec = asio::error::not_found;
        }

        return or_throw(yield, ec, CacheEntry{content.ts, parser.release()});
    }

private:
    asio::io_service& ios;
    unique_ptr<ipfs_cache::Injector>& injector;
    CacheControl cc;
};

//------------------------------------------------------------------------------
static
void serve( GenericConnection con
          , unique_ptr<ipfs_cache::Injector>& injector
          , Signal<void()>& close_connection_signal
          , asio::yield_context yield)
{
    auto close_connection_slot = close_connection_signal.connect([&con] {
        con.close();
    });

    for (;;) {
        sys::error_code ec;

        Request req;
        beast::flat_buffer buffer;
        http::async_read(con, buffer, req, yield[ec]);
        if (ec == asio::error::operation_aborted) {
            break;
        }

        if (req.method() == http::verb::connect) {
            return handle_connect_request(con, req, close_connection_signal, yield);
        }

        InjectorCacheControl cc(con.get_io_service(), injector, close_connection_signal);
        auto res = cc.fetch(req, yield[ec]);
        if (ec) {
            break;
        }

        // Forward back the response
        http::async_write(con, res, yield[ec]);
        if (ec) {
            break;
        }
    }
}

//------------------------------------------------------------------------------
static
void listen( OuiServiceServer& proxy_server
           , unique_ptr<ipfs_cache::Injector>& ipfs_cache_injector
           , Signal<void()>& shutdown_signal
           , asio::yield_context yield)
{
    auto stop_proxy_slot = shutdown_signal.connect([&proxy_server] {
        proxy_server.stop_listen();
    });

    asio::io_service& ios = proxy_server.get_io_service();

    sys::error_code ec;
    proxy_server.start_listen(yield[ec]);
    if (ec) {
        std::cerr << "Failed to setup ouiservice proxy server: " << ec.message() << endl;
        return;
    }

    WaitCondition shutdown_connections(ios);

    while (true) {
        GenericConnection connection = proxy_server.accept(yield[ec]);
        if (ec == boost::asio::error::operation_aborted) {
            break;
        } else if (ec) {
            if (!async_sleep(ios, std::chrono::milliseconds(100), shutdown_signal, yield)) {
                break;
            }
            continue;
        }

        asio::spawn(ios, [
            connection = std::move(connection),
            &ipfs_cache_injector,
            &shutdown_signal,
            lock = shutdown_connections.lock()
        ] (boost::asio::yield_context yield) mutable {
            serve(std::move(connection), ipfs_cache_injector, shutdown_signal, yield);
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
        ("listen-on-i2p",
         po::value<string>(),
         "Whether we should be listening on I2P (true/false)")
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

    if (!vm.count("listen-on-tcp") && !vm.count("listen-on-gnunet") && !vm.count("listen-on-i2p")) {
        cerr << "One or more of {listen-on-tcp,listen-on-gnunet,listen-on-i2p}"
             << " must be provided." << endl;
        cerr << desc << endl;
        return 1;
    }

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    // Unfortunately, Boost.ProgramOptions doesn't support arguments without
    // values in config files. Thus we need to force the 'listen-on-i2p' arg
    // to have one of the strings values "true" or "false".
    if (vm.count("listen-on-i2p")) {
        auto value = vm["listen-on-i2p"].as<string>();

        if (value != "" && value != "true" && value != "false") {
            cerr << "The listen-on-i2p parameter may be either 'true' or 'false'"
                 << endl;
            return 1;
        }
    }

    if (exists(REPO_ROOT/OUINET_PID_FILE)) {
        cerr << "Existing PID file " << REPO_ROOT/OUINET_PID_FILE
             << "; another injector process may be running"
             << ", otherwise please remove the file." << endl;
        return 1;
    }
    // Acquire a PID file for the life of the process
    util::PidFile pid_file(REPO_ROOT/OUINET_PID_FILE);



    // The io_service is required for all I/O
    asio::io_service ios;

    Signal<void()> shutdown_signal;



    auto ipfs_cache_injector
        = make_unique<ipfs_cache::Injector>(ios, (REPO_ROOT/"ipfs").native());

    auto shutdown_ipfs_slot = shutdown_signal.connect([&] {
        ipfs_cache_injector = nullptr;
    });

    // Although the IPNS ID is already in IPFS's config file,
    // this just helps put all info relevant to the user right in the repo root.
    auto ipns_id = ipfs_cache_injector->ipns_id();
    cout << "IPNS DB: " << ipns_id << endl;
    util::create_state_file(REPO_ROOT/"cache-ipns", ipns_id);




    OuiServiceServer proxy_server(ios);

    if (vm.count("listen-on-tcp")) {
        auto endpoint_setting = util::parse_endpoint(vm["listen-on-tcp"].as<string>());
        asio::ip::tcp::endpoint endpoint = boost::get<asio::ip::tcp::endpoint>(endpoint_setting);

        string endpoint_string = endpoint.address().to_string() + ":" + to_string(endpoint.port());
        cout << "TCP Address: " << endpoint_string << endl;
        util::create_state_file(REPO_ROOT/"endpoint-tcp", endpoint_string);

        std::unique_ptr<ouiservice::TcpOuiServiceServer> tcp_server = std::make_unique<ouiservice::TcpOuiServiceServer>(ios, endpoint);
        proxy_server.add(std::move(tcp_server));
    }

    if (vm.count("listen-on-i2p") && vm["listen-on-i2p"].as<string>() == "true") {
        std::shared_ptr<ouiservice::I2pOuiService> i2p_service = std::make_shared<ouiservice::I2pOuiService>((REPO_ROOT/"i2p").string(), ios);
        std::unique_ptr<ouiservice::I2pOuiServiceServer> i2p_server = i2p_service->build_server("i2p-private-key");

        auto ep = i2p_server->public_identity();
        cout << "I2P Public ID: " << ep << endl;
        util::create_state_file(REPO_ROOT/"endpoint-i2p", ep);

        proxy_server.add(std::move(i2p_server));
    }

/*
    if (vm.count("listen-on-gnunet")) {
        string port = vm["listen-on-gnunet"].as<string>();
        
        
    }
*/

    asio::spawn(ios, [
        &proxy_server,
        &ipfs_cache_injector,
        &shutdown_signal
    ] (asio::yield_context yield) {
        listen(proxy_server, ipfs_cache_injector, shutdown_signal, yield);
    });



    asio::signal_set signals(ios, SIGINT, SIGTERM);

    signals.async_wait([&shutdown_signal, &signals, &ios](const sys::error_code& ec, int signal_number) {
            cerr << "Got signal" << endl;
            shutdown_signal();

            signals.async_wait([](const sys::error_code& ec, int signal_number) {
                cerr << "Got second signal, terminating immediately" << endl;
                exit(1);
            });
        });

    ios.run();

    // TODO: Remove this once work on clean exit is done.
    cerr << "Clean exit" << endl;

    return EXIT_SUCCESS;
}
