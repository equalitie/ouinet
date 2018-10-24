#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
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
#include "generic_stream.h"
#include "split_string.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "full_duplex_forward.h"
#include "injector_config.h"
#include "authenticate.h"
#include "force_exit_on_signal.h"
#include "http_util.h"
#include "connection_pool.h"

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"

#include "util/timeout.h"
#include "util/crypto.h"

#include "logger.h"
#include "defer.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
// We are more interested in an ID generator that can be
// used concurrently and does not block by random pool exhaustion
// than we are in getting unpredictable IDs;
// thus we use a pseudo-random generator.
using uuid_generator = boost::uuids::random_generator_mt19937;
using Request     = http::request<http::string_body>;
using Response    = http::response<http::dynamic_body>;
using TCPLookup   = asio::ip::tcp::resolver::results_type;

struct PoolId {
    bool is_ssl;
    string host;

    bool operator<(const PoolId& other) const {
        return tie(is_ssl, host) < tie(other.is_ssl, other.host);
    }
};

using ConPool = ConnectionPool<>;
using Connection = ConPool::Connection;
using ConPools = map<PoolId, ConPool>;

static const boost::filesystem::path OUINET_PID_FILE = "pid";

//------------------------------------------------------------------------------
static
void handle_bad_request( GenericStream& con
                       , const Request& req
                       , string message
                       , Yield yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = message;
    res.prepare_payload();

    yield.log("=== Sending back response ===");
    yield.log(res);

    sys::error_code ec;
    http::async_write(con, res, yield[ec]);
}

//------------------------------------------------------------------------------
// Note: the connection is attempted towards
// the already resolved endpoints in `lookup`,
// only headers are used from `req`.
static
void handle_connect_request( GenericStream client_c
                           , const Request& req, const TCPLookup& lookup
                           , Signal<void()>& disconnect_signal
                           , Yield yield)
{
    sys::error_code ec;

    asio::io_service& ios = client_c.get_io_service();

    auto disconnect_client_slot = disconnect_signal.connect([&client_c] {
        client_c.close();
    });

    // Restrict connections to well-known ports.
    auto port = lookup.begin()->endpoint().port();  // all entries use same port
    // TODO: This is quite arbitrary;
    // enhance this filter or remove the restriction altogether.
    if (port != 80 && port != 443 && port != 8080 && port != 8443) {
        ec = asio::error::invalid_argument;
        auto ep = util::format_ep(lookup.begin()->endpoint());
        return handle_bad_request( client_c, req
                                 , "Illegal CONNECT target: " + ep
                                 , yield[ec]);
    }

    auto origin_c = connect_to_host( lookup, ios
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

    if (ec) {
        cerr << "Failed sending CONNECT response: " << ec.message() << endl;
        return;
    }

    full_duplex(move(client_c), move(origin_c), yield);
}

//------------------------------------------------------------------------------
static Request erase_hop_by_hop_headers(Request rq) {
    //// TODO
    //rq.erase(http::field::connection);
    //rq.erase(http::field::keep_alive);
    //rq.erase(http::field::public_);
    //rq.erase(http::field::transfer_encoding);
    //rq.erase(http::field::upgrade);
    rq.erase(http::field::proxy_authenticate);
    return rq;
}

//------------------------------------------------------------------------------
static
TCPLookup
resolve_target(const Request&, asio::io_service&, Signal<void()>&, Yield yield);

struct InjectorCacheControl {
public:
    unique_ptr<Connection> connect( asio::io_service& ios
                                  , const Request& rq
                                  , const util::url_match& url
                                  , Signal<void()>& abort_signal
                                  , Yield yield)
    {
        using ConP = unique_ptr<Connection>;

        sys::error_code ec;

        // Resolve target endpoint and check its validity.
        TCPLookup lookup(resolve_target( rq, ios, abort_signal
                                       , yield[ec]));

        if (ec) return or_throw<ConP>(yield, ec);

        auto socket = connect_to_host( lookup
                                     , ios
                                     , abort_signal
                                     , yield[ec]);

        if (ec) return or_throw<ConP>(yield, ec);

        GenericStream stream;

        if (url.scheme == "https") {
            stream = ssl::util::client_handshake( move(socket)
                                                , ssl_ctx
                                                , url.host
                                                , abort_signal
                                                , yield[ec]);

            if (ec) return or_throw<ConP>(yield, ec);
        }
        else {
            stream = move(socket);
        }

        return std::make_unique<Connection>(move(stream), boost::none);
    }

    // TODO: Replace this with cancellation support in which fetch_ operations
    // get a signal parameter
    InjectorCacheControl( asio::io_service& ios
                        , asio::ssl::context& ssl_ctx
                        , ConPools& connection_pools
                        , const InjectorConfig& config
                        , unique_ptr<CacheInjector>& injector
                        , uuid_generator& genuuid
                        , Signal<void()>& abort_signal)
        : ssl_ctx(ssl_ctx)
        , injector(injector)
        , config(config)
        , genuuid(genuuid)
        , cc("Ouinet Injector")
        , connection_pools(connection_pools)
    {
        // The following operations take care of adding or removing
        // a custom Ouinet HTTP response header with the injection identifier
        // to enable the tracking of this particular injection.
        // The header is added when fetching fresh content or retrieving from the cache,
        // (so it is sent to the client in both cases)
        // and it is removed just before saving to the cache
        // (though it is still used to create the descriptor).

        cc.fetch_fresh = [&] (const Request& rq_, Yield yield) {
            sys::error_code ec;

            string host = rq_[http::field::host].to_string();
            assert(!host.empty());

            // Parse the URL to tell HTTP/HTTPS, host, port.
            util::url_match url;

            if (!util::match_http_url(rq_.target().to_string(), url)) {
                return or_throw<Response>( yield
                                         , asio::error::operation_not_supported);
            }

            bool is_ssl = url.scheme == "https";

            PoolId pool_id{is_ssl, move(host)};

            auto& pool = connection_pools[pool_id];

            auto connection = pool.pop_front();

            auto on_exit = defer([&] {
                if (pool.empty()) connection_pools.erase(pool_id);
            });

            if (!connection) {
                connection = connect(ios, rq_, url, abort_signal, yield[ec]);
            }

            Request rq = erase_hop_by_hop_headers(rq_);
            rq.keep_alive(true);

            Response ret;

            if (!ec) ret = connection->request(rq, yield[ec]);

            // Add an injection identifier header.
            if (!ec) ret.set(http_::response_injection_id_hdr, to_string(genuuid()));

            if (!ec && ret.keep_alive() && rq_.keep_alive()) {
                // Note: we can't use the `pool` ref from above because that
                // pool may have been destroyed in the mean time.
                connection_pools[pool_id].push_back(move(connection));
            }

            return or_throw(yield, ec, move(ret));
        };

        cc.fetch_stored = [this](const Request& rq, Yield yield) {
            return this->fetch_stored(rq, yield);
        };

        cc.store = [this](const Request& rq, Response rs, Yield yield) {
            return this->insert_content(rq, rs, yield);
        };
    }

    Response fetch(const Request& rq, Yield yield)
    {
        return cc.fetch(rq, yield);
    }

private:
    Response insert_content(Request rq, Response rs, Yield yield)
    {
        if (!injector) return rs;

        // Recover and pop out synchronous injection toggle.
        bool sync = ( rq[http_::request_sync_injection_hdr]
                      == http_::request_sync_injection_true );

        // Recover and pop out injection identifier.
        auto id = rs[http_::response_injection_id_hdr].to_string();
        assert(!id.empty());

        // This injection code logs errors but does not propagate them.
        auto inject = [
            rq, rs,
            injector = injector.get(),
            db_type = config.default_db_type()
        ] (boost::asio::yield_context yield) mutable -> string {
            rq.erase(http_::request_sync_injection_hdr);

            sys::error_code ec;
            auto ret = injector->insert_content( move(rq)
                                               , move(rs)
                                               , db_type
                                               , yield[ec]);

            if (ec) {
                cout << "!Insert failed: " << rq.target()
                     << " " << ec.message() << endl;
            }

            return ret;
        };

        // Proceed to or program the real injection.
        LOG_DEBUG((sync ? "Sync inject: " : "Async inject: ")
                  , rq.target(), " ", id);

        if (sync) {
            // Zlib-compress descriptor, Base64-encode and put in header.
            auto desc_data = inject(yield);
            auto compressed_desc = util::zlib_compress(move(desc_data));
            auto encoded_desc = util::base64_encode(move(compressed_desc));
            rs.set(http_::response_descriptor_hdr, move(encoded_desc));
        } else {
            asio::spawn(asio::yield_context(yield), inject);
        }

        return rs;
    }

    CacheEntry
    fetch_stored(const Request& rq, asio::yield_context yield)
    {
        if (!injector)
            return or_throw<CacheEntry>( yield
                                       , asio::error::operation_not_supported);

        // TODO: use string_view
        return injector->get_content( rq.target().to_string()
                                    , config.default_db_type()
                                    , yield);
    }

private:
    asio::ssl::context& ssl_ctx;
    unique_ptr<CacheInjector>& injector;
    const InjectorConfig& config;
    uuid_generator& genuuid;
    CacheControl cc;
    string last_host; // A host to which the below connection was established
    ConPools& connection_pools;
};

//------------------------------------------------------------------------------
// Resolve request target address, check whether it is valid
// and return lookup results.
// If not valid, set error code and send an error message over `con`
// (the returned lookup may not be usable then).
static
TCPLookup
resolve_target( const Request& req
              , GenericStream& con
              , Signal<void()>& shutdown_signal
              , Yield yield)
{
    sys::error_code ec;
    auto lookup = resolve_target( req
                                , con.get_io_service()
                                , shutdown_signal
                                , yield[ec]);
    if (!ec)
        return lookup;

    // Prepare and send error message to `con`.
    string host, err;
    tie(host, ignore) = util::get_host_port(req);

    if (ec == asio::error::netdb_errors::host_not_found)
        err = "Could not resolve host: " + host;
    else if (ec == asio::error::invalid_argument)
        err = "Illegal target host: " + host;
    else
        err = "Unknown resolver error: " + ec.message();

    handle_bad_request( con, req, err
                      , yield[ec].tag("handle_bad_request"));
    return or_throw<TCPLookup>(yield, ec);
}

// Resolve request target address, check whether it is valid
// and return lookup results.
// If not valid, set error code
// (the returned lookup may not be usable then).
static
TCPLookup
resolve_target( const Request& req
              , asio::io_service& ios
              , Signal<void()>& shutdown_signal
              , Yield yield)
{
    TCPLookup lookup;
    sys::error_code ec;

    string host, port;
    tie(host, port) = util::get_host_port(req);

    // First test trivial cases (like "localhost" or "127.1.2.3").
    bool local = util::is_localhost(host);

    // Resolve address and also use result for more sophisticaded checking.
    if (!local)
        lookup = util::tcp_async_resolve( host, port
                                        , ios
                                        , shutdown_signal
                                        , yield[ec]);
    if (ec) {
        return or_throw<TCPLookup>(yield, ec);
    }

    // Test non-trivial cases (like "[0::1]" or FQDNs pointing to loopback).
    for (auto r : lookup)
        if ((local = util::is_localhost(r.endpoint().address().to_string())))
            break;

    if (local) {
        ec = asio::error::invalid_argument;
        return or_throw<TCPLookup>(yield, ec);
    }

    return or_throw(yield, ec, move(lookup));
}

//------------------------------------------------------------------------------
static
void serve( InjectorConfig& config
          , uint64_t connection_id
          , GenericStream con
          , asio::ssl::context& ssl_ctx
          , unique_ptr<CacheInjector>& injector
          , ConPools& connection_pools
          , uuid_generator& genuuid
          , Signal<void()>& close_connection_signal
          , asio::yield_context yield_)
{
    auto close_connection_slot = close_connection_signal.connect([&con] {
        con.close();
    });

    InjectorCacheControl cc( con.get_io_service()
                           , ssl_ctx
                           , connection_pools
                           , config
                           , injector
                           , genuuid
                           , close_connection_signal);

    for (;;) {
        sys::error_code ec;

        Request req;
        beast::flat_buffer buffer;
        http::async_read(con, buffer, req, yield_[ec]);

        Yield yield(con.get_io_service(), yield_, util::str('C', connection_id));

        if (ec) break;

        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done"); });

        if (!authenticate(req, con, config.credentials(), yield[ec].tag("auth"))) {
            continue;
        }

        TCPLookup lookup;
        bool proxy = (req.find(http_::request_version_hdr) == req.end());
        if (proxy || req.method() == http::verb::connect) {
            // Resolve target endpoint and check its validity.
            lookup = resolve_target( req, con, close_connection_signal
                                   , yield[ec]);
            if (ec) continue;  // error message already sent to `con`
        }

        if (req.method() == http::verb::connect) {
            return handle_connect_request( move(con)
                                         , req, lookup
                                         , close_connection_signal
                                         , yield.tag("handle_connect"));
        }

        // Check for a Ouinet version header hinting us on
        // whether to behave like an injector or a proxy.
        Response res;
        auto req2(req);
        if (proxy) {
            // No Ouinet header, behave like a (non-caching) proxy.
            // TODO: Maybe reject requests for HTTPS URLS:
            // we are perfectly able to handle them (and do verification locally),
            // but the client should be using a CONNECT request instead!
            // TODO: Reuse the connection c response contains "Connection: keep-alive"
            GenericStream c;
            res = fetch_http_page( con.get_io_service()
                                 , c
                                 , ssl_ctx
                                 , erase_hop_by_hop_headers(move(req2))
                                 , lookup
                                 , default_timeout::fetch_http()
                                 , close_connection_signal
                                 , yield[ec].tag("fetch_http_page"));
        } else {
            // Ouinet header found, behave like a Ouinet injector.
            req2.erase(http_::request_version_hdr);  // do not propagate or cache the header
            res = cc.fetch(req2, yield[ec].tag("cache_control.fetch"));
            res.keep_alive(true);
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

        if (!req.keep_alive()) {
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
    uuid_generator genuuid;

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

    uint64_t next_connection_id = 0;

    ConPools connection_pools;

    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(asio::ssl::verify_peer);

    while (true) {
        GenericStream connection = proxy_server.accept(yield[ec]);
        if (ec == boost::asio::error::operation_aborted) {
            break;
        } else if (ec) {
            if (!async_sleep(ios, std::chrono::milliseconds(100), shutdown_signal, yield)) {
                break;
            }
            continue;
        }

        uint64_t connection_id = next_connection_id++;

        asio::spawn(ios, [
            connection = std::move(connection),
            &ssl_ctx,
            &cache_injector,
            &shutdown_signal,
            &config,
            &genuuid,
            &connection_pools,
            connection_id,
            lock = shutdown_connections.lock()
        ] (boost::asio::yield_context yield) mutable {
            serve( config
                 , connection_id
                 , std::move(connection)
                 , ssl_ctx
                 , cache_injector
                 , connection_pools
                 , genuuid
                 , shutdown_signal
                 , yield);
        });
    }
}

//------------------------------------------------------------------------------
int main(int argc, const char* argv[])
{
    util::crypto_init();

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

    unique_ptr<CacheInjector> cache_injector;

    if (config.cache_enabled()) {
        cache_injector = make_unique<CacheInjector>
                                ( ios
                                , config.bt_private_key()
                                , config.repo_root());

        auto shutdown_ipfs_slot = shutdown_signal.connect([&] {
            cache_injector = nullptr;
        });

        // Although the IPNS ID is already in IPFS's config file,
        // this just helps put all info relevant to the user right in the repo root.
        auto ipns_id = cache_injector->ipfs_id();
        LOG_DEBUG("IPNS DB: " + ipns_id);
        util::create_state_file(config.repo_root()/"cache-ipns", ipns_id);
    }

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
