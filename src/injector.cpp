#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>  // for atexit()

#include "cache/cache_injector.h"

#include "namespaces.h"
#include "util.h"
#include "fetch_http_page.h"
#include "connect_to_host.h"
#include "default_timeout.h"
#include "cache_control.h"
#include "generic_connection.h"
#include "split_string.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "full_duplex_forward.h"
#include "injector_config.h"
#include "authenticate.h"
#include "force_exit_on_signal.h"
#include "request_routing.h"

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"

#include "util/timeout.h"

#include "logger.h"
#include "defer.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;
using Response    = http::response<http::dynamic_body>;

static const boost::filesystem::path OUINET_PID_FILE = "pid";

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

    // Split CONNECT target in host and port (443 i.e. HTTPS by default).
    auto hp = req["host"];
    if (hp.empty() && req.version() == 10)  // HTTP/1.0 proxy client with no ``Host:``
        hp = req.target();
    auto pos = hp.rfind(':');
    string host, port;
    if (pos != string::npos) {
        host = hp.substr(0, pos).to_string();
        port = hp.substr(pos + 1).to_string();
    } else {
        host = hp.to_string();
        port = "443";  // HTTPS port by default
    }
    // Restrict connections towards certain hosts and ports.
    // TODO: Enhance this filter.
    if (util::is_localhost(host, ios, disconnect_signal, yield[ec])
        || (port != "80" && port != "443" && port != "8080" && port != "8443")) {
        ec = asio::error::invalid_argument;
        return handle_bad_request( client_c, req
                                 , "Illegal CONNECT target: " + port
                                 , yield[ec]);
    }

    auto origin_c = connect_to_host( ios, host, port
                                   , default_timeout::tcp_connect()
                                   , disconnect_signal, yield[ec]);

    if (ec) {
        return handle_bad_request( client_c, req
                                 , "Failed to connect to origin: " + ec.message()
                                 , yield[ec]);
    }

    auto disconnect_origin_slot = disconnect_signal.connect([&origin_c] {
        origin_c.close();
    });

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    // No ``res.prepare_payload()`` since no payload is allowed for CONNECT:
    // <https://tools.ietf.org/html/rfc7231#section-6.3.1>.

    http::async_write(client_c, res, yield[ec]);

    if (ec) return fail(ec, "sending connect response");

    full_duplex(client_c, origin_c, yield);
}

//------------------------------------------------------------------------------
struct InjectorCacheControl {
public:
    // TODO: Replace this with cancellation support in which fetch_ operations
    // get a signal parameter
    InjectorCacheControl( asio::io_service& ios
                        , unique_ptr<CacheInjector>& injector
                        , Signal<void()>& abort_signal)
        : ios(ios)
        , injector(injector)
        , cc("Ouinet Injector")
    {
        cc.fetch_fresh = [&ios, &abort_signal]
                         (const Request& rq, asio::yield_context yield) {
            return fetch_http_page( ios
                                  , rq
                                  , default_timeout::fetch_http()
                                  , abort_signal
                                  , yield);
        };

        cc.fetch_stored = [this](const Request& rq, asio::yield_context yield) {
            return this->fetch_stored(rq, yield);
        };

        cc.store = [this](const Request& rq, const Response& rs) {
            this->insert_content(rq, rs);
        };
    }

    Response fetch(const Request& rq, Yield yield)
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
    unique_ptr<CacheInjector>& injector;
    CacheControl cc;
};

//------------------------------------------------------------------------------
static
void serve( InjectorConfig& config
          , GenericConnection con
          , unique_ptr<CacheInjector>& injector
          , Signal<void()>& close_connection_signal
          , asio::yield_context yield_)
{
    auto close_connection_slot = close_connection_signal.connect([&con] {
        con.close();
    });

    for (;;) {
        sys::error_code ec;

        Request req;
        beast::flat_buffer buffer;
        http::async_read(con, buffer, req, yield_[ec]);

        Yield yield(con.get_io_service(), yield_);

        if (ec) break;

        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done"); });

        if (!authenticate(req, con, config.credentials(), yield[ec].tag("auth"))) {
            continue;
        }

        if (req.method() == http::verb::connect) {
            return handle_connect_request( con
                                         , req
                                         , close_connection_signal
                                         , yield.tag("handle_connect"));
        }

        // Check for a Ouinet version header hinting us on
        // whether to behave like an injector or a proxy.
        Response res;
        auto req2(req);
        auto ouinet_version_hdr = req2.find(request_version_hdr);
        if (ouinet_version_hdr == req2.end()) {
            // No Ouinet header, behave like a (non-caching) proxy.
            // TODO: Maybe reject requests for HTTPS URLS:
            // we are perfectly able to handle them (and do verification locally),
            // but the client should be using a CONNECT request instead!
            res = fetch_http_page( con.get_io_service()
                                 , req2
                                 , default_timeout::fetch_http()
                                 , close_connection_signal
                                 , yield[ec].tag("fetch_http_page"));
        } else {
            // Ouinet header found, behave like a Ouinet injector.
            req2.erase(ouinet_version_hdr);  // do not propagate or cache the header
            InjectorCacheControl cc(con.get_io_service(), injector, close_connection_signal);
            res = cc.fetch(req2, yield[ec].tag("cache_control.fetch"));
        }
        if (ec) {
            handle_bad_request( con, req
                              , "Failed to retrieve content from origin: " + ec.message()
                              , yield[ec].tag("handle_bad_request"));
            continue;
        }


        yield.log("=== Sending back response ===");
        yield.log(res.base());

        // Forward back the response
        http::async_write(con, res, yield[ec].tag("write_response"));

        if (ec) break;

        if (!res.keep_alive()) {
            con.close();
            break;
        }
    }
}

//------------------------------------------------------------------------------
static
void listen( InjectorConfig& config
           , OuiServiceServer& proxy_server
           , unique_ptr<CacheInjector>& cache_injector
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
            &cache_injector,
            &shutdown_signal,
            &config,
            lock = shutdown_connections.lock()
        ] (boost::asio::yield_context yield) mutable {
            serve( config
                 , std::move(connection)
                 , cache_injector
                 , shutdown_signal
                 , yield);
        });
    }
}

//------------------------------------------------------------------------------
int main(int argc, const char* argv[])
{
    InjectorConfig config;

    try {
        config = InjectorConfig(argc, argv);
    }
    catch(const exception& e) {
        cerr << e.what() << endl;
        cerr << InjectorConfig::options_description() << endl;
        return 1;
    }

    if (config.is_help()) {
        cout << InjectorConfig::options_description() << endl;
        return EXIT_SUCCESS;
    }

    if (config.open_file_limit()) {
        increase_open_file_limit(*config.open_file_limit());
    }

    if (exists(config.repo_root()/OUINET_PID_FILE)) {
      LOG_ABORT("Existing PID file ", config.repo_root()/OUINET_PID_FILE,
                "; another injector process may be running" ,
                ", otherwise please remove the file.\n");
        return 1;
    }
    // Acquire a PID file for the life of the process
    static const auto pid_file_path = config.repo_root()/OUINET_PID_FILE;
    util::PidFile pid_file(pid_file_path);
    // Force removal of PID file on abnormal exit
    std::atexit([] {
            if (!exists(pid_file_path)) return;
            cerr << "Warning: not a clean exit" << endl;
            remove(pid_file_path);
        });

    // The io_service is required for all I/O
    asio::io_service ios;

    Signal<void()> shutdown_signal;

    auto cache_injector
        = make_unique<CacheInjector>(ios, (config.repo_root()/"ipfs").native());

    auto shutdown_ipfs_slot = shutdown_signal.connect([&] {
        cache_injector = nullptr;
    });

    // Although the IPNS ID is already in IPFS's config file,
    // this just helps put all info relevant to the user right in the repo root.
    auto ipns_id = cache_injector->id();
    LOG_DEBUG("IPNS DB: " + ipns_id);
    util::create_state_file(config.repo_root()/"cache-ipns", ipns_id);

    OuiServiceServer proxy_server(ios);

    if (config.tcp_endpoint()) {
        tcp::endpoint endpoint = *config.tcp_endpoint();
        cout << "TCP Address: " << endpoint << endl;

        util::create_state_file( config.repo_root()/"endpoint-tcp"
                               , util::str(endpoint));

        proxy_server.add(make_unique<ouiservice::TcpOuiServiceServer>(ios, endpoint));
    }

    if (config.listen_on_i2p()) {
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((config.repo_root()/"i2p").string(), ios);
        std::unique_ptr<ouiservice::I2pOuiServiceServer> i2p_server = i2p_service->build_server("i2p-private-key");

        auto ep = i2p_server->public_identity();
        cout << "I2P Public ID: " << ep << endl;
        util::create_state_file(config.repo_root()/"endpoint-i2p", ep);

        proxy_server.add(std::move(i2p_server));
    }

    asio::spawn(ios, [
        &proxy_server,
        &cache_injector,
        &config,
        &shutdown_signal
    ] (asio::yield_context yield) {
        listen( config
              , proxy_server
              , cache_injector
              , shutdown_signal
              , yield);
    });

    asio::signal_set signals(ios, SIGINT, SIGTERM);

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&shutdown_signal, &signals, &ios, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            shutdown_signal();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    ios.run();

    return EXIT_SUCCESS;
}
