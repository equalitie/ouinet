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
#include "origin_pools.h"

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"
#include "ouiservice/tls.h"
#include "ssl/ca_certificate.h"
#include "ssl/util.h"

#include "util/timeout.h"
#include "util/crypto.h"

#include "logger.h"
#include "defer.h"
#include "http_util.h"

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
using TcpLookup   = asio::ip::tcp::resolver::results_type;

static const fs::path OUINET_TLS_CERT_FILE = "tls-cert.pem";
static const fs::path OUINET_TLS_KEY_FILE = "tls-key.pem";
static const fs::path OUINET_TLS_DH_FILE = "tls-dh.pem";


//------------------------------------------------------------------------------
boost::optional<Response> version_error_response( const Request& rq
                                                , string_view oui_version)
{
    unsigned version = util::parse_num<unsigned>(oui_version, 0);

    unsigned supported_version
        = util::parse_num<unsigned>(http_::request_version_hdr_current, -1);

    assert(supported_version != (unsigned) -1);

    if (version == supported_version) {
        return boost::none;
    }

    Response res{http::status::bad_request, rq.version()};
    res.set(http::field::server, OUINET_INJECTOR_SERVER_STRING);
    res.keep_alive(false);

    if (version < supported_version) {
        res.set( http_::response_error_hdr
               , http_::response_error_hdr_version_too_low);
    }
    else if (version > supported_version) {
        res.set( http_::response_error_hdr
               , http_::response_error_hdr_version_too_high);
    }

    return res;
}

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
// Resolve request target address, check whether it is valid
// and return lookup results.
// If not valid, set error code
// (the returned lookup may not be usable then).
static
TcpLookup
resolve_target( const Request& req
              , asio::io_service& ios
              , Cancel& cancel
              , Yield yield)
{
    TcpLookup lookup;
    sys::error_code ec;

    string host, port;
    tie(host, port) = util::get_host_port(req);

    // First test trivial cases (like "localhost" or "127.1.2.3").
    bool local = util::is_localhost(host);

    // Resolve address and also use result for more sophisticaded checking.
    if (!local)
        lookup = util::tcp_async_resolve( host, port
                                        , ios
                                        , cancel
                                        , yield[ec]);

    if (ec) return or_throw<TcpLookup>(yield, ec);

    // Test non-trivial cases (like "[0::1]" or FQDNs pointing to loopback).
    for (auto r : lookup)
        if ((local = util::is_localhost(r.endpoint().address().to_string())))
            break;

    if (local) {
        ec = asio::error::invalid_argument;
        return or_throw<TcpLookup>(yield, ec);
    }

    return or_throw(yield, ec, move(lookup));
}

//------------------------------------------------------------------------------
// Note: the connection is attempted towards
// the already resolved endpoints in `lookup`,
// only headers are used from `req`.
static
void handle_connect_request( GenericStream client_c
                           , const Request& req
                           , Cancel& cancel
                           , Yield yield)
{
    sys::error_code ec;

    asio::io_service& ios = client_c.get_io_service();

    auto disconnect_client_slot = cancel.connect([&client_c] {
        client_c.close();
    });

    TcpLookup lookup = resolve_target(req, ios, cancel, yield[ec]);

    if (ec) {
        // Prepare and send error message to `con`.
        string host, err;
        tie(host, ignore) = util::get_host_port(req);

        if (ec == asio::error::netdb_errors::host_not_found)
            err = "Could not resolve host: " + host;
        else if (ec == asio::error::invalid_argument)
            err = "Illegal target host: " + host;
        else
            err = "Unknown resolver error: " + ec.message();

        handle_bad_request( client_c, req, err
                          , yield[ec].tag("handle_bad_request"));

        return;
    }

    assert(!lookup.empty());

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
                                   , cancel, yield[ec]);

    if (ec) {
        return handle_bad_request( client_c, req
                                 , "Failed to connect to origin: " + ec.message()
                                 , yield[ec]);
    }

    auto disconnect_origin_slot = cancel.connect([&origin_c] {
        origin_c.close();
    });

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    // No ``res.prepare_payload()`` since no payload is allowed for CONNECT:
    // <https://tools.ietf.org/html/rfc7231#section-6.3.1>.

    http::async_write(client_c, res, yield[ec]);

    if (ec) {
        yield.log("Failed sending CONNECT response: ", ec.message());
        return;
    }

    full_duplex(move(client_c), move(origin_c), yield);
}

//------------------------------------------------------------------------------
struct InjectorCacheControl {
    using Connection = OriginPools::Connection;

public:
    unique_ptr<Connection> connect( asio::io_service& ios
                                  , const Request& rq
                                  , Cancel& cancel
                                  , Yield yield)
    {
        using ConP = unique_ptr<Connection>;

        // Parse the URL to tell HTTP/HTTPS, host, port.
        util::url_match url;

        if (!util::match_http_url(rq.target(), url)) {
            return or_throw<ConP>( yield
                                 , asio::error::operation_not_supported);
        }

        sys::error_code ec;

        // Resolve target endpoint and check its validity.
        TcpLookup lookup = resolve_target(rq, ios, cancel, yield[ec]);

        if (ec) return or_throw<ConP>(yield, ec);

        auto socket = connect_to_host( lookup
                                     , ios
                                     , cancel
                                     , yield[ec]);

        if (ec) return or_throw<ConP>(yield, ec);

        GenericStream stream;

        if (url.scheme == "https") {
            stream = ssl::util::client_handshake( move(socket)
                                                , ssl_ctx
                                                , url.host
                                                , cancel
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
                        , OriginPools& origin_pools
                        , const InjectorConfig& config
                        , unique_ptr<CacheInjector>& injector
                        , uuid_generator& genuuid
                        , Cancel& cancel)
        : ios(ios)
        , ssl_ctx(ssl_ctx)
        , injector(injector)
        , config(config)
        , genuuid(genuuid)
        , cc(ios, OUINET_INJECTOR_SERVER_STRING)
        , origin_pools(origin_pools)
    {
        // The following operations take care of adding or removing
        // a custom Ouinet HTTP response header with the injection identifier
        // to enable the tracking of this particular injection.
        // The header is added when fetching fresh content or retrieving from the cache,
        // (so it is sent to the client in both cases)
        // and it is removed just before saving to the cache
        // (though it is still used to create the descriptor).

        cc.fetch_fresh = [&] (const Request& rq, Cancel& c, Yield y) {
            return fetch_fresh(rq, c, y);
        };

        cc.fetch_stored = [&](const Request& rq, Cancel& c, Yield y) {
            return fetch_stored(rq, c, y);
        };

        cc.store = [&](const Request& rq, Response rs, Cancel& /* TODO */, Yield y) {
            return store(rq, rs, y);
        };
    }

    Response fetch(const Request& rq, Yield yield)
    {
        Cancel cancel;

        bool timed_out = false;

        WatchDog wd(ios, chrono::seconds(3*60), [&] {
            timed_out = true;
            cancel();
        });

        sys::error_code ec;
        Response response = cc.fetch(rq, cancel, yield[ec]);

        if (timed_out) ec = asio::error::timed_out;

        return or_throw(yield, ec, move(response));
    }

    Response fetch_fresh(const Request& rq_, Cancel& cancel, Yield yield) {
        sys::error_code ec;

        auto connection = origin_pools.get_connection(rq_);

        if (!connection) {
            connection = connect(ios, rq_, cancel, yield[ec].tag("connect"));
        }

        if (ec) return or_throw<Response>(yield, ec);

        Request rq = util::to_origin_request(rq_);
        rq.keep_alive(true);
        Response ret = connection->request(rq, cancel, yield[ec]);

        if (ec) return or_throw<Response>(yield, ec);

        // Prevent others from inserting ouinet specific header fields.
        ret = util::remove_ouinet_fields(move(ret));

        // Add an injection identifier header
        // to enable the client to track injection state.
        ret.set(http_::response_injection_id_hdr, to_string(genuuid()));

        if (ret.keep_alive() && rq_.keep_alive()) {
            origin_pools.insert_connection(rq_, move(connection));
        }

        // Prevent origin from inserting ouinet specific header fields.
        return ret;
    }

private:
    CacheEntry
    fetch_stored(const Request& rq, Cancel& cancel, Yield yield)
    {
        if (!injector)
            return or_throw<CacheEntry>( yield
                                       , asio::error::operation_not_supported);

        sys::error_code ec;

        // TODO: use string_view
        auto ret = injector->get_content( key_from_http_req(rq)
                                        , config.default_index_type()
                                        , cancel
                                        , yield[ec]);

        if (ec) return or_throw(yield, ec, move(ret.second));

        // Prevent others from inserting ouinet specific header fields.
        ret.second.response = util::remove_ouinet_fields(move(ret.second.response));

        // Add an injection identifier header
        // to enable the client to track injection state.
        ret.second.response.set(http_::response_injection_id_hdr, ret.first);

        return move(ret.second);
    }

    Response store(Request rq, Response rs, Yield yield)
    {
        if (!injector) return rs;

        // Recover synchronous injection toggle.
        bool sync = ( rq[http_::request_sync_injection_hdr]
                      == http_::request_sync_injection_true );

        // Recover injection identifier.
        auto id = rs[http_::response_injection_id_hdr].to_string();
        assert(!id.empty());

        // This injection code logs errors but does not propagate them
        // (the `desc_data` field is set to the empty string).
        auto index_type = config.default_index_type();
        auto inject = [
            rq, rs, id, index_type,
            injector = injector.get()
        ] (boost::asio::yield_context yield) mutable
          -> CacheInjector::InsertionResult {
            // Pop out Ouinet internal HTTP headers.
            rq = util::to_cache_request(move(rq));
            rs = util::to_cache_response(move(rs));

            sys::error_code ec;
            auto ret = injector->insert_content( id, rq, rs
                                               , index_type
                                               , yield[ec]);

            if (ec) {
                cout << "!Insert failed: " << rq.target()
                     << " " << ec.message() << endl;
                ret.desc_data = "";
            }

            return ret;
        };

        // Program or proceed to the real injection.

        if (!sync) {
            LOG_DEBUG("Async inject: ", rq.target(), " ", id);
            asio::spawn(asio::yield_context(yield), inject);
            return rs;
        }

        LOG_DEBUG("Sync inject: ", rq.target(), " ", id);
        auto ins = inject(yield);
        if (ins.desc_data.length() == 0)
            return rs;  // insertion failed

        // Zlib-compress descriptor, Base64-encode and put in header.
        auto compressed_desc = util::zlib_compress(move(ins.desc_data));
        auto encoded_desc = util::base64_encode(move(compressed_desc));
        rs.set(http_::response_descriptor_hdr, move(encoded_desc));
        // Add descriptor storage link as is.
        rs.set(http_::response_descriptor_link_hdr, move(ins.desc_link));
        // Add Base64-encoded reinsertion data (if any).
        if (ins.index_ins_data.length() > 0) {
            auto encoded_insd = util::base64_encode(move(ins.index_ins_data));
            rs.set( http_::response_insert_hdr_pfx + IndexName.at(index_type)
                  , move(encoded_insd));
        }
        return rs;
    }

private:
    asio::io_service& ios;
    asio::ssl::context& ssl_ctx;
    unique_ptr<CacheInjector>& injector;
    const InjectorConfig& config;
    uuid_generator& genuuid;
    CacheControl cc;
    OriginPools& origin_pools;
};

//------------------------------------------------------------------------------
static
void serve( InjectorConfig& config
          , uint64_t connection_id
          , GenericStream con
          , asio::ssl::context& ssl_ctx
          , unique_ptr<CacheInjector>& injector
          , OriginPools& origin_pools
          , uuid_generator& genuuid
          , Cancel& cancel
          , asio::yield_context yield_)
{
    auto close_connection_slot = cancel.connect([&con] {
        con.close();
    });

    InjectorCacheControl cc( con.get_io_service()
                           , ssl_ctx
                           , origin_pools
                           , config
                           , injector
                           , genuuid
                           , cancel);

    for (;;) {
        sys::error_code ec;

        Request req;
        beast::flat_buffer buffer;
        http::async_read(con, buffer, req, yield_[ec]);

        if (ec) break;

        Yield yield(con.get_io_service(), yield_, util::str('C', connection_id));

        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done"); });

        if (!authenticate(req, con, config.credentials(), yield[ec].tag("auth"))) {
            continue;
        }

        if (req.method() == http::verb::connect) {
            return handle_connect_request( move(con)
                                         , req
                                         , cancel
                                         , yield.tag("handle_connect"));
        }

        auto version_hdr_i = req.find(http_::request_version_hdr);

        // Check for a Ouinet version header hinting us on
        // whether to behave like an injector or a proxy.
        bool proxy = (version_hdr_i == req.end());

        Response res;

        if (proxy) {
            // No Ouinet header, behave like a (non-caching) proxy.
            // TODO: Maybe reject requests for HTTPS URLS:
            // we are perfectly able to handle them (and do verification locally),
            // but the client should be using a CONNECT request instead!
            res = cc.fetch_fresh(req, cancel, yield[ec].tag("fetch_proxy"));
        } else {
            // Ouinet header found, behave like a Ouinet injector.
            auto opt_err_res = version_error_response(req, version_hdr_i->value());

            if (opt_err_res) {
                res = *opt_err_res;
            }
            else {
                auto req2 = util::to_injector_request(req);  // sanitize
                req2.keep_alive(req.keep_alive());
                res = cc.fetch(req2, yield[ec].tag("cache_control.fetch"));
                res.keep_alive(req.keep_alive());
            }
        }

        if (ec) {
            handle_bad_request( con, req
                              , "Failed to retrieve content from origin: " + ec.message()
                              , yield[ec].tag("handle_bad_request"));
            continue;
        }

        yield.log("=== Sending back response ===");
        yield.log(res.base());

        // Note: Not 100% sure about this, but sometimes we got a HTTP 1.1
        // response that did not contain the `Connection: close` header field
        // nor any indicationi about the body size. Since in such cases we
        // don't close connections to clients, clients keep waiting for the
        // body indefinitely. The `prepare_payload` functions sets the body
        // size so as to avoid such situations.
        res.prepare_payload();

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
           , Cancel& cancel
           , asio::yield_context yield)
{
    uuid_generator genuuid;

    auto stop_proxy_slot = cancel.connect([&proxy_server] {
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

    OriginPools origin_pools;

    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(asio::ssl::verify_peer);

    while (true) {
        GenericStream connection = proxy_server.accept(yield[ec]);
        if (ec == boost::asio::error::operation_aborted) {
            break;
        } else if (ec) {
            if (!async_sleep(ios, std::chrono::milliseconds(100), cancel, yield)) {
                break;
            }
            continue;
        }

        uint64_t connection_id = next_connection_id++;

        asio::spawn(ios, [
            connection = std::move(connection),
            &ssl_ctx,
            &cache_injector,
            &cancel,
            &config,
            &genuuid,
            &origin_pools,
            connection_id,
            lock = shutdown_connections.lock()
        ] (boost::asio::yield_context yield) mutable {
            serve( config
                 , connection_id
                 , std::move(connection)
                 , ssl_ctx
                 , cache_injector
                 , origin_pools
                 , genuuid
                 , cancel
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

    // Create or load the TLS certificate.
    auto tls_certificate = get_or_gen_tls_cert<EndCertificate>
        ( "localhost"
        , config.repo_root() / OUINET_TLS_CERT_FILE
        , config.repo_root() / OUINET_TLS_KEY_FILE
        , config.repo_root() / OUINET_TLS_DH_FILE );

    // The io_service is required for all I/O
    asio::io_service ios;

    Cancel cancel;

    unique_ptr<CacheInjector> cache_injector;
    Cancel::Connection shutdown_ipfs_slot;

    if (config.cache_enabled()) {
        cache_injector = make_unique<CacheInjector>
                                ( ios
                                , config.bt_private_key()
                                , config.repo_root());

        shutdown_ipfs_slot = cancel.connect([&] {
            cache_injector = nullptr;
        });

        // Although the IPNS ID is already in IPFS's config file,
        // this just helps put all info relevant to the user right in the repo root.
        auto ipns_id = cache_injector->ipfs_id();
        LOG_DEBUG("IPNS Index: " + ipns_id);
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

    asio::ssl::context ssl_context{asio::ssl::context::tls_server};
    if (config.tls_endpoint()) {
        ssl_context = ssl::util::get_server_context
            ( tls_certificate->pem_certificate()
            , tls_certificate->pem_private_key()
            , tls_certificate->pem_dh_param());

        tcp::endpoint endpoint = *config.tls_endpoint();
        cout << "TLS Address: " << endpoint << endl;
        util::create_state_file( config.repo_root()/"endpoint-tls"
                               , util::str(endpoint));

        auto base = make_unique<ouiservice::TcpOuiServiceServer>(ios, endpoint);
        proxy_server.add(make_unique<ouiservice::TlsOuiServiceServer>(move(base), ssl_context));
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
        &cancel
    ] (asio::yield_context yield) {
        listen( config
              , proxy_server
              , cache_injector
              , cancel
              , yield);
    });

    asio::signal_set signals(ios, SIGINT, SIGTERM);

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&cancel, &signals, &ios, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            cancel();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    ios.run();

    return EXIT_SUCCESS;
}
