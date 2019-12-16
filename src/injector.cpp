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
#include <chrono>
#include <ctime>
#include <iostream>
#include <fstream>
#include <string>

#include "cache/http_sign.h"

#include "bittorrent/dht.h"

#include "namespaces.h"
#include "util.h"
#include "fetch_http_page.h"
#include "http_forward.h"
#include "connect_to_host.h"
#include "default_timeout.h"
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
#include "session.h"

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/lampshade.h"
#include "ouiservice/pt-obfs2.h"
#include "ouiservice/pt-obfs3.h"
#include "ouiservice/pt-obfs4.h"
#include "ouiservice/tcp.h"
#include "ouiservice/utp.h"
#include "ouiservice/tls.h"
#include "ouiservice/bep5/server.h"
#include "ssl/ca_certificate.h"
#include "ssl/util.h"

#include "util/timeout.h"
#include "util/atomic_file.h"
#include "util/crypto.h"
#include "util/bytes.h"
#include "util/file_io.h"
#include "util/file_posix_with_offset.h"

#include "logger.h"
#include "defer.h"
#include "http_util.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using udp         = asio::ip::udp;
namespace bt = bittorrent;
// We are more interested in an ID generator that can be
// used concurrently and does not block by random pool exhaustion
// than we are in getting unpredictable IDs;
// thus we use a pseudo-random generator.
using uuid_generator = boost::uuids::random_generator_mt19937;
using Request     = http::request<http::string_body>;
using Response    = http::response<http::dynamic_body>;
using TcpLookup   = asio::ip::tcp::resolver::results_type;
using ResponseWithFileBody = http::response<http::basic_file_body<
    util::file_posix_with_offset>>;

static const fs::path OUINET_TLS_CERT_FILE = "tls-cert.pem";
static const fs::path OUINET_TLS_KEY_FILE = "tls-key.pem";
static const fs::path OUINET_TLS_DH_FILE = "tls-dh.pem";


//------------------------------------------------------------------------------
static
void handle_bad_request( GenericStream& con
                       , const Request& req
                       , string message
                       , Yield yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, OUINET_INJECTOR_SERVER_STRING);
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
              , asio::executor exec
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
                                        , exec
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

    asio::executor exec = client_c.get_executor();

    auto disconnect_client_slot = cancel.connect([&client_c] {
        client_c.close();
    });

    TcpLookup lookup = resolve_target(req, exec, cancel, yield[ec]);

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

    auto origin_c = connect_to_host( lookup, exec
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
    GenericStream connect( const Request& rq
                         , Cancel& cancel
                         , Yield yield)
    {
        // Parse the URL to tell HTTP/HTTPS, host, port.
        util::url_match url;

        if (!util::match_http_url(rq.target(), url)) {
            return or_throw<GenericStream>( yield
                                          , asio::error::operation_not_supported);
        }

        sys::error_code ec;

        // Resolve target endpoint and check its validity.
        TcpLookup lookup = resolve_target(rq, executor, cancel, yield[ec]);

        if (ec) return or_throw<GenericStream>(yield, ec);

        auto socket = connect_to_host( lookup
                                     , executor
                                     , cancel
                                     , yield[ec]);

        if (ec) return or_throw<GenericStream>(yield, ec);

        if (url.scheme == "https") {
            auto c = ssl::util::client_handshake( move(socket)
                                                , ssl_ctx
                                                , url.host
                                                , cancel
                                                , yield[ec]);

            return or_throw(yield, ec, move(c));
        } else {
            return GenericStream(move(socket));
        }
    }

    // TODO: Replace this with cancellation support in which fetch_ operations
    // get a signal parameter
    InjectorCacheControl( asio::executor executor
                        , asio::ssl::context& ssl_ctx
                        , OriginPools& origin_pools
                        , const InjectorConfig& config
                        , uuid_generator& genuuid)
        : insert_id(to_string(genuuid()))
        , insert_ts(chrono::seconds(time(nullptr)).count())
        , executor(move(executor))
        , ssl_ctx(ssl_ctx)
        , config(config)
        , genuuid(genuuid)
        , origin_pools(origin_pools)
    {
    }

    void inject_fresh( GenericStream& con
                     , Request rq
                     , Cancel& cancel
                     , Yield yield)
    {
        yield.log("Injection begin");

        sys::error_code ec;

        // Pop out Ouinet internal HTTP headers.
        rq = util::to_cache_request(move(rq));

        auto orig_con = get_connection(rq, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        // Send HTTP request to origin.
        http_forward_request( orig_con, con, util::to_origin_request(rq)
                            , cancel, yield[ec]);  // .tag("fetch_injector")
        if (ec) yield.log("Failed to send request: ", ec.message());
        return_or_throw_on_error(yield, cancel, ec);

        Session orig_sess = Session::create(move(orig_con), cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        cache::session_flush_signed( orig_sess, con
                                   , rq, insert_id, insert_ts
                                   , config.cache_private_key()
                                   , cancel, yield[ec]);  // .tag("fetch_injector")

        if (ec) yield.log("Injection failed: ", ec.message());
        return_or_throw_on_error(yield, cancel, ec);
        yield.log("Injection end");  // TODO: report whether inject or just fwd

        auto rsh = http::response<http::empty_body>(orig_sess.response_header());
        keep_connection(rq, rsh, move(orig_sess));
    }

    bool fetch( GenericStream& con
              , const Request& rq
              , Cancel cancel
              , Yield yield)
    {
        sys::error_code ec;
        bool keep_alive = rq.keep_alive();
        inject_fresh(con, rq, cancel, yield[ec]);
        // TODO: keep_alive should consider response as well
        return or_throw(yield, ec, keep_alive);
    }

public:
    Connection get_connection(const Request& rq_, Cancel& cancel, Yield yield) {
        Connection connection;
        sys::error_code ec;

        auto maybe_connection = origin_pools.get_connection(rq_);
        if (maybe_connection) {
            connection = std::move(*maybe_connection);
        } else {
            auto stream = connect(rq_, cancel, yield[ec].tag("connect"));

            if (ec) return or_throw<Connection>(yield, ec);

            connection = origin_pools.wrap(rq_, std::move(stream));
        }
        return connection;
    }

    template<class Response, class Connection>
    bool keep_connection(const Request& rq, const Response& rs, Connection con) {
        if (!con.is_open()) return false;

        if (!rs.keep_alive() || !rq.keep_alive()) {
            con.close();
            return false;
        }

        return true;
    }

private:
    std::string insert_id;
    std::chrono::seconds::rep insert_ts;
    asio::executor executor;
    asio::ssl::context& ssl_ctx;
    const InjectorConfig& config;
    uuid_generator& genuuid;
    OriginPools& origin_pools;
};

//------------------------------------------------------------------------------
bool is_request_to_this(const Request& rq) {
    if (rq.method() == http::verb::connect) return false;
    // TODO: Check this one
    if (rq.method() == http::verb::options) return true;
    // Check that the request is *not* in 'origin-form'
    // https://tools.ietf.org/html/rfc7230#section-5.3
    return rq.target().starts_with('/');
}

//------------------------------------------------------------------------------
void handle_request_to_this(Request& rq, GenericStream& con, Yield yield)
{
    if (rq.target() == "/api/ok") {
        http::response<http::empty_body> rs{http::status::ok, rq.version()};

        rs.set(http::field::server, OUINET_INJECTOR_SERVER_STRING);
        rs.set(http::field::content_type, "text/html");
        rs.keep_alive(rq.keep_alive());
        rs.prepare_payload();

        http::async_write(con, rs, yield);
        return;
    }

    handle_bad_request(con, rq, "Unknown injector request", yield);
}

//------------------------------------------------------------------------------
static
void serve( InjectorConfig& config
          , uint64_t connection_id
          , GenericStream con
          , asio::ssl::context& ssl_ctx
          , OriginPools& origin_pools
          , uuid_generator& genuuid
          , Cancel& cancel
          , asio::yield_context yield_)
{
    auto close_connection_slot = cancel.connect([&con] {
        con.close();
    });

    InjectorCacheControl cc( con.get_executor()
                           , ssl_ctx
                           , origin_pools
                           , config
                           , genuuid);

    for (;;) {
        sys::error_code ec;

        Request req;
        beast::flat_buffer buffer;
        http::async_read(con, buffer, req, yield_[ec]);

        if (ec) break;

        Yield yield(con.get_executor(), yield_, util::str('C', connection_id));

        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done"); });

        if (is_request_to_this(req)) {
            handle_request_to_this(req, con, yield[ec]);
            if (ec || !req.keep_alive()) break;
            continue;
        }

        if (!authenticate(req, con, config.credentials(), yield[ec].tag("auth"))) {
            continue;
        }

        if (req.method() == http::verb::connect) {
            return handle_connect_request( move(con)
                                         , req
                                         , cancel
                                         , yield.tag("handle_connect"));
        }

        auto version_hdr_i = req.find(http_::protocol_version_hdr);

        // Check for a Ouinet version header hinting us on
        // whether to behave like an injector or a proxy.
        bool proxy = (version_hdr_i == req.end());

        bool keep_alive = req.keep_alive();

        if (proxy) {
            // No Ouinet header, behave like a (non-caching) proxy.
            // TODO: Maybe reject requests for HTTPS URLS:
            // we are perfectly able to handle them (and do verification locally),
            // but the client should be using a CONNECT request instead!
            using RespFromH = http::response<http::empty_body>;
            RespFromH res;
            auto orig_con = cc.get_connection(req, cancel, yield[ec]);
            size_t forwarded = 0;
            if (!ec) {
                // TODO: Refactor with session response flushing.
                string chunk_exts;
                auto hproc = [&] (auto inh, auto&, auto) {
                    // Prevent others from inserting ouinet specific header fields.
                    auto outh = util::remove_ouinet_fields(move(inh));
                    yield.log("=== Sending back proxy response ===");
                    yield.log(outh);
                    return outh;
                };
                auto xproc = [&] (auto exts, auto&, auto) {
                    chunk_exts = move(exts);  // save exts for next chunk
                };
                ProcDataFunc<asio::const_buffer> dproc = [&] (auto ind, auto&, auto) {
                    forwarded += ind.size();
                    ProcDataFunc<asio::const_buffer>::result_type ret;
                    if (asio::buffer_size(ind) > 0) {
                        ret = {move(ind), move(chunk_exts)};
                        chunk_exts = {};  // only send extensions in first output chunk
                    }  // keep extensions when last chunk (size 0) was received
                    return ret;  // just pass data on
                };
                ProcTrailFunc tproc = [&] (auto intr, auto&, auto) {
                    ProcTrailFunc::result_type ret{move(intr), move(chunk_exts)};
                    return ret;  // leave trailers untouched
                };
                res = RespFromH(http_forward( orig_con, con
                                            , util::to_origin_request(req)
                                            , hproc, xproc, dproc, tproc
                                            , cancel, yield[ec].tag("fetch_proxy")));
            }
            if (ec) {
                handle_bad_request( con, req
                                  , "Failed to retrieve content from origin: " + ec.message()
                                  , yield[ec].tag("handle_bad_request"));
                continue;
            }
            yield.log("Forwarded data bytes: ", forwarded);
            keep_alive = cc.keep_connection(req, res, move(orig_con));
        }
        else {
            // Ouinet header found, behave like a Ouinet injector.
            auto opt_err_res = util::http_proto_version_error( req, version_hdr_i->value()
                                                             , OUINET_INJECTOR_SERVER_STRING);

            if (opt_err_res) {
                http::async_write(con, *opt_err_res, yield[ec]);
            }
            else {
                auto req2 = util::to_injector_request(req);  // sanitize
                req2.keep_alive(req.keep_alive());
                keep_alive = cc.fetch( con
                                     , req2
                                     , cancel
                                     , yield[ec].tag("cache_control.fetch"));
            }
        }

        if (ec || !keep_alive) {
            con.close();
            break;
        }
    }
}

//------------------------------------------------------------------------------
static
void listen( InjectorConfig& config
           , OuiServiceServer& proxy_server
           , Cancel& cancel
           , asio::yield_context yield)
{
    uuid_generator genuuid;

    auto stop_proxy_slot = cancel.connect([&proxy_server] {
        proxy_server.stop_listen();
    });

    asio::executor exec = proxy_server.get_executor();

    sys::error_code ec;
    proxy_server.start_listen(yield[ec]);
    if (ec) {
        std::cerr << "Failed to setup ouiservice proxy server: " << ec.message() << endl;
        return;
    }

    WaitCondition shutdown_connections(exec);

    uint64_t next_connection_id = 0;

    OriginPools origin_pools;

    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(asio::ssl::verify_peer);

    ssl::util::load_tls_ca_certificates(ssl_ctx, config.tls_ca_cert_store_path());

    while (true) {
        GenericStream connection = proxy_server.accept(yield[ec]);
        if (ec == boost::asio::error::operation_aborted) {
            break;
        } else if (ec) {
            if (!async_sleep(exec, std::chrono::milliseconds(100), cancel, yield)) {
                break;
            }
            continue;
        }

        uint64_t connection_id = next_connection_id++;

        asio::spawn(exec, [
            connection = std::move(connection),
            &ssl_ctx,
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
        return 1;
    }

    if (config.is_help()) {
        cout << config.options_description() << endl;
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

    // The io_context is required for all I/O
    asio::io_context ioc;
    asio::executor ex = ioc.get_executor();

    shared_ptr<bt::MainlineDht> bt_dht_ptr;

    auto bittorrent_dht = [&bt_dht_ptr, &config, ex] {
        if (!config.bittorrent_endpoint() || bt_dht_ptr) return bt_dht_ptr;
        bt_dht_ptr = make_shared<bt::MainlineDht>(ex);
        bt_dht_ptr->set_endpoints({*config.bittorrent_endpoint()});
        assert(!bt_dht_ptr->local_endpoints().empty());
        return bt_dht_ptr;
    };

    OuiServiceServer proxy_server(ex);

    if (config.tcp_endpoint()) {
        tcp::endpoint endpoint = *config.tcp_endpoint();
        LOG_INFO("TCP address: ", endpoint);

        util::create_state_file( config.repo_root()/"endpoint-tcp"
                               , util::str(endpoint));

        proxy_server.add(make_unique<ouiservice::TcpOuiServiceServer>(ex, endpoint));
    }

    auto read_ssl_certs = [&] {
        return ssl::util::get_server_context
            ( tls_certificate->pem_certificate()
            , tls_certificate->pem_private_key()
            , tls_certificate->pem_dh_param());
    };

    asio::ssl::context ssl_context{asio::ssl::context::tls_server};
    if (config.tcp_tls_endpoint()) {
        ssl_context = read_ssl_certs();

        tcp::endpoint endpoint = *config.tcp_tls_endpoint();
        LOG_INFO("TCP/TLS address: ", endpoint);
        util::create_state_file( config.repo_root()/"endpoint-tcp-tls"
                               , util::str(endpoint));

        auto base = make_unique<ouiservice::TcpOuiServiceServer>(ex, endpoint);
        proxy_server.add(make_unique<ouiservice::TlsOuiServiceServer>(ex, move(base), ssl_context));
    }

    if (config.utp_endpoint()) {
        udp::endpoint endpoint = *config.utp_endpoint();
        LOG_INFO("uTP address: ", endpoint);

        util::create_state_file( config.repo_root()/"endpoint-utp"
                               , util::str(endpoint));

        auto srv = make_unique<ouiservice::UtpOuiServiceServer>(ex, endpoint);
        proxy_server.add(move(srv));
    }

    if (config.utp_tls_endpoint()) {
        ssl_context = read_ssl_certs();

        udp::endpoint endpoint = *config.utp_tls_endpoint();

        auto base = make_unique<ouiservice::UtpOuiServiceServer>(ex, endpoint);

        auto local_ep = base->local_endpoint();

        if (local_ep) {
            LOG_INFO("uTP/TLS address: ", *local_ep);
            util::create_state_file( config.repo_root()/"endpoint-utp-tls"
                                   , util::str(*local_ep));
            proxy_server.add(make_unique<ouiservice::TlsOuiServiceServer>(ex, move(base), ssl_context));

        } else {
            LOG_ERROR("Failed to start uTP/TLS service on ", *config.utp_tls_endpoint());
        }
    }

    if (config.bep5_injector_swarm_name()) {
        ssl_context = read_ssl_certs();
        auto dht = bittorrent_dht();
        assert(dht);
        assert(!dht->local_endpoints().empty());
        proxy_server.add(make_unique<ouiservice::Bep5Server>
                (move(dht), &ssl_context, *config.bep5_injector_swarm_name()));
    }

/*
    if (config.lampshade_endpoint()) {
        tcp::endpoint endpoint = *config.lampshade_endpoint();
        util::create_state_file( config.repo_root()/"endpoint-lampshade"
                               , util::str(endpoint));

        unique_ptr<ouiservice::LampshadeOuiServiceServer> server =
            make_unique<ouiservice::LampshadeOuiServiceServer>(ios, endpoint, config.repo_root()/"lampshade-server");
        LOG_INFO("Lampshade address: ", endpoint, ",key=", server->public_key());

        proxy_server.add(std::move(server));
    }
*/

    if (config.obfs2_endpoint()) {
        tcp::endpoint endpoint = *config.obfs2_endpoint();
        LOG_INFO("obfs2 address: ", endpoint);
        util::create_state_file( config.repo_root()/"endpoint-obfs2"
                               , util::str(endpoint));

        proxy_server.add(make_unique<ouiservice::Obfs2OuiServiceServer>(ioc, endpoint, config.repo_root()/"obfs2-server"));
    }

    if (config.obfs3_endpoint()) {
        tcp::endpoint endpoint = *config.obfs3_endpoint();
        LOG_INFO("obfs3 address: ", endpoint);
        util::create_state_file( config.repo_root()/"endpoint-obfs3"
                               , util::str(endpoint));

        proxy_server.add(make_unique<ouiservice::Obfs3OuiServiceServer>(ioc, endpoint, config.repo_root()/"obfs3-server"));
    }

    if (config.obfs4_endpoint()) {
        tcp::endpoint endpoint = *config.obfs4_endpoint();

        util::create_state_file( config.repo_root()/"endpoint-obfs4"
                               , util::str(endpoint));

        unique_ptr<ouiservice::Obfs4OuiServiceServer> server =
            make_unique<ouiservice::Obfs4OuiServiceServer>(ioc, endpoint, config.repo_root()/"obfs4-server");
        asio::spawn(ex, [
            obfs4 = server.get(),
            endpoint
        ] (asio::yield_context yield) {
            sys::error_code ec;
            obfs4->wait_for_running(yield[ec]);
            if (!ec) {
                LOG_INFO("obfs4 address: ", endpoint, ",", obfs4->connection_arguments());
            }
        });
        proxy_server.add(std::move(server));
    }

    if (config.listen_on_i2p()) {
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((config.repo_root()/"i2p").string(), ex);
        std::unique_ptr<ouiservice::I2pOuiServiceServer> i2p_server = i2p_service->build_server("i2p-private-key");

        auto ep = i2p_server->public_identity();
        LOG_INFO("I2P public ID: ", ep);
        util::create_state_file(config.repo_root()/"endpoint-i2p", ep);

        proxy_server.add(std::move(i2p_server));
    }

    LOG_INFO("HTTP signing public key (Ed25519): ", config.cache_private_key().public_key());

    Cancel cancel;

    asio::spawn(ex, [
        &ex,
        &proxy_server,
        &config,
        &cancel
    ] (asio::yield_context yield) {
        sys::error_code ec;
        listen(config, proxy_server, cancel, yield[ec]);
    });

    asio::signal_set signals(ex, SIGINT, SIGTERM);

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&cancel, &signals, &force_exit, &bt_dht_ptr]
                       (const sys::error_code& ec, int signal_number) {
            if (bt_dht_ptr) {
                bt_dht_ptr->stop();
                bt_dht_ptr = nullptr;
            }
            cancel();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    ioc.run();

    return EXIT_SUCCESS;
}
