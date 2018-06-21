#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
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
#include "cache_control.h"
#include "generic_connection.h"
#include "split_string.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "full_duplex_forward.h"
#include "injector_config.h"
#include "authenticate.h"
#include "force_exit_on_signal.h"

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"

#include "util/signal.h"

using namespace std;
using namespace ouinet;
namespace ssl = boost::asio::ssl;

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
    auto pos = hp.rfind(':');
    string host, port;
    if (pos != string::npos) {
        host = hp.substr(0, pos).to_string();
        port = hp.substr(pos + 1).to_string();
    } else {
        host = hp.to_string();
        port = "443";  // HTTPS port by default
    }

    auto origin_c = connect_to_host(ios, host, port, disconnect_signal, yield[ec]);

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

static
GenericConnection ssl_client_handshake( GenericConnection& con
                                      , const string& host
                                      , asio::yield_context yield) {
    // SSL contexts do not seem to be reusable.
    ssl::context ssl_context{ssl::context::sslv23};
    ssl_context.set_default_verify_paths();
    ssl_context.set_verify_mode(ssl::verify_peer);
    ssl_context.set_verify_callback(ssl::rfc2818_verification(host));

    sys::error_code ec;

    // When we adopt Boost >= 1.67
    // (which enables moving ownership of the underlying connection into ``ssl::stream``),
    // these will be ``ssl::stream<GenericConnection>``
    // and we can move `con` (a `GenericConnection&&`).
    auto ssl_sock = make_unique<ssl::stream<GenericConnection&>>(con, ssl_context);
    ssl_sock->async_handshake(ssl::stream_base::client, yield[ec]);
    if (ec) return or_throw<GenericConnection>(yield, ec);

    static const auto ssl_shutter = [](ssl::stream<GenericConnection&>& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericConnection(move(ssl_sock), move(ssl_shutter));
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
    {
        cc.fetch_fresh = [this, &ios, &abort_signal]
                         (const Request& rq, asio::yield_context yield) {
            auto target = rq.target().to_string();
            sys::error_code ec;

            // Parse the URL to tell HTTP/HTTPS, host, port.
            util::url_match url;
            if (!util::match_http_url(target, url)) {
                ec = asio::error::operation_not_supported;  // unsupported URL
                return or_throw<Response>(yield, ec);
            }
            string url_port;
            bool ssl(false);
            if (url.port.length() > 0)
                url_port = url.port;
            else if (url.scheme == "https") {
                url_port = "443";
                ssl = true;
            } else  // url.scheme == "http"
                url_port = "80";

            auto con = connect_to_host(ios, url.host, url_port, abort_signal, yield[ec]);
            if (ec) return or_throw<Response>(yield, ec);

            auto close_con_slot = abort_signal.connect([&con] {
                con.close();
            });

            // When we adopt Boost >= 1.67
            // `con` will be moved into `ssl_handshake()`
            // and we will be able to just replace `con` here with the SSL-enabled connection.
            // For the moment we keep both here and
            // decide which one to use according to `ssl`.
            GenericConnection ssl_con;
            if (ssl) {
                ssl_con = ssl_client_handshake(con, url.host, yield[ec]);
                if (ec) {
                    cerr << "SSL client handshake error: "
                         << url.host << ": " << ec.message() << endl;
                    return or_throw<Response>(yield, ec);
                }
            }

            // Now that we have a connection to the origin
            // we can send a non-proxy request to it
            // (i.e. with target "/foo..." and not "http://example.com/foo...").
            // Actually some web servers do not like the full form.
            Request origin_rq(rq);
            origin_rq.target(target.substr(target.find( url.path
                                                      // Length of "http://" or "https://",
                                                      // do not fail on "http(s)://FOO/FOO".
                                                      , url.scheme.length() + 3)));
            return fetch_http_page(ios, ssl? ssl_con : con, origin_rq, yield);
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
    unique_ptr<CacheInjector>& injector;
    CacheControl cc;
    //RateLimiter _rate_limiter;
};

//------------------------------------------------------------------------------
static
void serve( InjectorConfig& config
          , GenericConnection con
          , unique_ptr<CacheInjector>& injector
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

        if (ec) break;

        if (!authenticate(req, con, config.credentials(), yield[ec])) {
            continue;
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
        cerr << "Existing PID file " << config.repo_root()/OUINET_PID_FILE
             << "; another injector process may be running"
             << ", otherwise please remove the file." << endl;
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
    cout << "IPNS DB: " << ipns_id << endl;
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
