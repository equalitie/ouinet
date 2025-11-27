#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <string>

#include "cache/http_sign.h"

#include "namespaces.h"
#include "util.h"
#include "connect_to_host.h"
#include "default_timeout.h"
#include "generic_stream.h"
#include "split_string.h"
#include "async_sleep.h"
#include "bittorrent/mainline_dht.h"
#ifndef __WIN32
#include "increase_open_file_limit.h"
#endif
#include "full_duplex_forward.h"
#include "injector.h"
#include "authenticate.h"
#include "http_util.h"
#include "http_logger.h"
#include "origin_pools.h"
#include "session.h"

#include "ouiservice.h"
#ifdef __EXPERIMENTAL__
#  include "ouiservice/i2p.h"
#  include "ouiservice/lampshade.h"
#  include "ouiservice/pt-obfs2.h"
#  include "ouiservice/pt-obfs3.h"
#  include "ouiservice/pt-obfs4.h"
#endif // ifdef __EXPERIMENTAL__
#include "ouiservice/tcp.h"
#include "ouiservice/utp.h"
#include "ouiservice/tls.h"
#include "ouiservice/bep5/server.h"
#include "ssl/ca_certificate.h"
#include "ssl/util.h"

#include "util/timeout.h"
#include "util/atomic_file.h"
#include "util/crypto.h"
#include "util/dns.h"
#include "util/bytes.h"
#include "util/file_io.h"
#include "util/yield.h"

#include "logger.h"
#include "defer.h"
#include "http_util.h"

#include "cxx/metrics.h"

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
using ouinet::util::AsioExecutor;

static const fs::path OUINET_TLS_CERT_FILE = "tls-cert.pem";
static const fs::path OUINET_TLS_KEY_FILE = "tls-key.pem";
static const fs::path OUINET_TLS_DH_FILE = "tls-dh.pem";

// TODO: Get rid of this
static bool g_allow_private_targets = false;
static bool g_do_doh = true;

//------------------------------------------------------------------------------
template<class Res>
static
void send_response( GenericStream& con
                  , const Res& res
                  , YieldContext yield)
{
    yield.log("=== Sending back response ===");
    yield.log(res);

    util::http_reply(con, res, yield.native());
}

static
void handle_error( GenericStream& con
                 , const Request& req
                 , http::status status
                 , const string& proto_error
                 , const string& message
                 , YieldContext yield)
{
    auto res = util::http_error( req.keep_alive(), status
                               , OUINET_INJECTOR_SERVER_STRING, proto_error, message);
    send_response(con, res, yield);
}

static
void handle_error( GenericStream& con
                 , const Request& req
                 , http::status status
                 , const string& message
                 , YieldContext yield)
{
    return handle_error(con, req, status, "", message, yield);
}

static
void handle_no_proxy( GenericStream& con
                    , const Request& req
                    , YieldContext yield)
{
    return handle_error( con, req, http::status::forbidden
                       , http_::response_error_hdr_proxy_disabled, "Proxy disabled"
                       , yield);
}


//------------------------------------------------------------------------------
// Resolve request target address, check whether it is valid
// and return lookup results.
// If not valid, set error code
// (the returned lookup may not be usable then).
TcpLookup
ouinet::resolve_target(const http::request_header<>& req
                      , bool allow_private_targets
                      , bool do_doh
                      , AsioExecutor exec
                      , Cancel& cancel
                      , YieldContext yield)
{
    TcpLookup lookup;
    sys::error_code ec;

    string host, port;
    tie(host, port) = util::get_host_port(req);

    // First test trivial cases (like "localhost" or "127.1.2.3").
    bool local = boost::regex_match(host, util::localhost_rx);
    bool priv = boost::regex_match(host, util::private_addr_rx);

    // Resolve address and also use result for more sophisticaded checking.
    if (!local && (!priv || allow_private_targets))
    {
        lookup = do_doh
               ? util::resolve_tcp_doh( host, port, cancel, yield[ec] )
               : util::resolve_tcp_async( host, port
                                        , exec
                                        , cancel
                                        , yield[ec].native());
    }

    if (ec) return or_throw<TcpLookup>(yield, ec);

    // Test non-trivial cases (like "[0::1]" or FQDNs pointing to loopback).
    for (auto r : lookup)
    {
        if ((local = boost::regex_match(r.endpoint().address().to_string()
                                        , util::localhost_rx)))
            break;
        if ((priv = boost::regex_match(r.endpoint().address().to_string()
                                      , util::private_addr_rx)))
            if (!allow_private_targets)
                break;
    }

    if (local || (priv && !allow_private_targets))
    {
        ec = asio::error::invalid_argument;
        return or_throw<TcpLookup>(yield, ec);
    }

    return or_throw(yield, ec, move(lookup));
}

//------------------------------------------------------------------------------
// Note: the connection is attempted towards
// the already resolved endpoints in `lookup`,
// only headers are used from `req`.
//
// `client_c_rbuf` contains data already read from `client_c`
// but not yet processed.
static
void handle_connect_request( GenericStream client_c
                           , beast::flat_buffer client_c_rbuf
                           , const Request& req
                           , Cancel& cancel
                           , YieldContext yield)
{
    sys::error_code ec;

    AsioExecutor exec = client_c.get_executor();

    auto disconnect_client_slot = cancel.connect([&client_c] {
        client_c.close();
    });

    TcpLookup lookup = resolve_target( req
                                     , g_allow_private_targets
                                     , g_do_doh
                                     , exec
                                     , cancel, yield[ec].tag("resolve"));

    if (ec) {
        sys::error_code he_ec;
        string host;
        tie(host, ignore) = util::get_host_port(req);

        if (ec == asio::error::invalid_argument)
            return handle_error( client_c, req, http::status::bad_request
                               , "Illegal target host: " + host
                               , yield[he_ec].tag("handle_no_host_error"));

        return handle_error( client_c, req, http::status::bad_gateway
                           , http_::response_error_hdr_retrieval_failed
                           , (ec == asio::error::netdb_errors::host_not_found)
                             ? ("Could not resolve host: " + host)
                             : ("Unknown resolver error: " + ec.message())
                           , yield[he_ec].tag("handle_resolve_error"));
    }

    assert(!lookup.empty());

    // Restrict connections to well-known ports.
    auto port = lookup.begin()->endpoint().port();  // all entries use same port
    // TODO: This is quite arbitrary;
    // enhance this filter or remove the restriction altogether.
    if (port != 80 && port != 443 && port != 8080 && port != 8443) {
        ec = asio::error::invalid_argument;
        auto ep = util::format_ep(lookup.begin()->endpoint());
        return handle_error( client_c, req
                           , http::status::forbidden
                           , http_::response_error_hdr_target_not_allowed
                           , "Illegal CONNECT target: " + ep
                           , yield[ec].tag("handle_bad_port_error"));
    }

    yield.log("BEGIN");

    // Remember to always set `ec` before return in case of error,
    // or the wrong error code will be reported.
    size_t fwd_bytes_c2o = 0, fwd_bytes_o2c = 0;
    auto log_result = defer([&] {
        yield.log("END; ec=", ec, " fwd_bytes_c2o=", fwd_bytes_c2o, " fwd_bytes_o2c=", fwd_bytes_o2c);
    });

    auto origin_c = yield[ec].tag("connect").run([&] (auto y) {
        return connect_to_host( lookup, exec, default_timeout::tcp_connect()
                              , cancel, y);
    });

    if (ec) {
        sys::error_code he_ec;
        return handle_error( client_c, req
                           , http::status::bad_gateway
                           , http_::response_error_hdr_retrieval_failed
                           , "Failed to connect to origin: " + ec.message()
                           , yield[he_ec].tag("handle_connect_error"));
    }

    auto disconnect_origin_slot = cancel.connect([&origin_c] {
        origin_c.close();
    });

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.prepare_payload();

    send_response(client_c, res, yield[ec].tag("write_res"));

    if (ec) {
        yield.log("Failed sending CONNECT response; ec=", ec);
        return;
    }

    // First queue unused but already read data back into the client connnection.
    if (client_c_rbuf.size() > 0) client_c.put_back(client_c_rbuf.data(), ec);
    assert(!ec);

    // Forward the rest of data in both directions.
    std::tie(fwd_bytes_c2o, fwd_bytes_o2c) =
        full_duplex(move(client_c), move(origin_c), cancel, yield[ec].tag("full_duplex"));

    return or_throw(yield, ec);
}

//------------------------------------------------------------------------------
class InjectorCacheControl {
    using Connection = OriginPools::Connection;

    GenericStream connect( const Request& rq
                         , Cancel& cancel
                         , YieldContext yield)
    {
        // Parse the URL to tell HTTP/HTTPS, host, port.
        auto url = util::Url::from(rq.target());

        if (!url) {
            yield.log("Unsupported target URL");
            return or_throw<GenericStream>( yield
                                          , asio::error::operation_not_supported);
        }

        sys::error_code ec;

        // Resolve target endpoint and check its validity.
        TcpLookup lookup = resolve_target( rq
                                         , g_allow_private_targets
                                         , g_do_doh
                                         , executor
                                         , cancel, yield[ec]);

        if (ec) return or_throw<GenericStream>(yield, ec);

        auto socket = connect_to_host( lookup
                                     , executor
                                     , cancel
                                     , yield[ec].native());

        if (ec) return or_throw<GenericStream>(yield, ec);

        if (url->scheme == "https") {
            auto c = ssl::util::client_handshake( move(socket)
                                                , ssl_ctx
                                                , url->host
                                                , cancel
                                                , yield[ec].native());

            return or_throw(yield, ec, move(c));
        } else {
            return GenericStream(move(socket));
        }
    }

public:
    // TODO: Replace this with cancellation support in which fetch_ operations
    // get a signal parameter
    InjectorCacheControl( AsioExecutor executor
                        , asio::ssl::context& ssl_ctx
                        , OriginPools& origin_pools
                        , const InjectorConfig& config
                        , uuid_generator& genuuid)
        : executor(move(executor))
        , ssl_ctx(ssl_ctx)
        , config(config)
        , genuuid(genuuid)
        , origin_pools(origin_pools)
    {
    }

private:
    void inject_fresh( GenericStream& con
                     , const Request& cache_rq
                     , bool rq_keep_alive
                     , Cancel& cancel
                     , YieldContext yield)
    {
        yield.log("BEGIN");

        // Remember to always set before return in case of error,
        // or the wrong error code will be reported.
        sys::error_code ec;
        size_t fwd_bytes = 0;
        auto log_result = defer([&] {
            yield.log("END; ec=", ec, " fwd_bytes=", fwd_bytes);
        });

        Session orig_sess;
        {
            Cancel timeout_cancel(cancel);

            // Start a short timeout for initial fetch.
            auto fetch_wd = watch_dog(executor, default_timeout::fetch_http(), [&] { timeout_cancel(); });

            auto orig_con = get_connection(cache_rq, timeout_cancel, yield.tag("connect")[ec]);
            if (ec = compute_error_code(ec, cancel, fetch_wd)) {
                yield.log("Failed to get connection; ec=", ec);
                return or_throw(yield, ec);
            }

            // Send HTTP request to origin.
            auto orig_rq = util::to_origin_request(cache_rq);
            orig_rq.keep_alive(true);  // regardless of what client wants
            yield[ec].tag("request").run([&] (auto y) {
                util::http_request(orig_con, orig_rq, timeout_cancel, y);
            });
            if (ec = compute_error_code(ec, cancel, fetch_wd)) {
                yield.log("Failed to send request; ec=", ec);
                return or_throw(yield, ec);
            }

            Session::reader_uptr sig_reader;
            auto cache_rq_method = cache_rq.method();
            if (cache_rq_method == http::verb::get || cache_rq_method == http::verb::head) {
                auto insert_id = to_string(genuuid());
                auto insert_ts = chrono::seconds(time(nullptr)).count();
                sig_reader = make_unique<cache::SigningReader>
                    (move(orig_con), cache_rq, move(insert_id), insert_ts, config.cache_private_key());
            } else {
                // Responses of unsafe or uncacheable requests should not be cached.
                yield.log("Not signing response: not a GET or HEAD request");
                sig_reader = make_unique<http_response::Reader>(move(orig_con));
            }

            orig_sess = yield[ec].tag("read_hdr").run([&] (auto y) {
                return Session::create( move(sig_reader), cache_rq_method == http::verb::head
                                      , timeout_cancel, y);
            });
            if (ec = compute_error_code(ec, cancel, fetch_wd)) {
                yield.log("Failed to process response head; ec=", ec);
                return or_throw(yield, ec);
            }
        }

        // Start a longer timeout for the main forwarding between origin and user,
        // and make it trigger even if the connection is moving data,
        // e.g. to avoid HTTP tar pits or endless transfers
        // which do not make much sense for Injector (the user may choose Proxy for those).
        auto overlong_wd = watch_dog(executor, chrono::hours(24), [&] { con.close(); });

        // Keep origin connection if the origin wants to.
        auto rs_keep_alive = orig_sess.response_header().keep_alive();
        // Keep client connection if the client wants to.
        orig_sess.response_header().keep_alive(rq_keep_alive);

        yield.log("=== Sending back injector response ===");
        yield.log(orig_sess.response_header());

        yield.tag("flush")[ec].run([&] (auto y) {
            orig_sess.flush_response(cancel, y, [&con, &fwd_bytes] (auto&& part, auto& cc, auto yy) {
                sys::error_code ee;
                part.async_write(con, cc, yy[ee]);
                return_or_throw_on_error(yy, cc, ee);
                if (auto b = part.as_body())
                    fwd_bytes += b->size();
                else if (auto cb = part.as_chunk_body())
                    fwd_bytes += cb->size();
            }, default_timeout::activity());
        });
        if (ec = compute_error_code(ec, cancel, overlong_wd)) {
            yield.log("Failed to process response; ec=", ec);
            return or_throw(yield, ec);
        } else {
            http_logger.log(druid, cache_rq, orig_sess, fwd_bytes);
        }

        keep_connection_if(move(orig_sess), rs_keep_alive);
    }

public:
    void fetch( GenericStream& con
              , Request rq
              , Cancel cancel
              , YieldContext yield)
    {
        sys::error_code ec;
        bool rq_keep_alive = rq.keep_alive();

        // Get DRUID before the Ouinet headers are removed.
        auto dr_it = rq.find(http_::request_druid_hdr);
        if (dr_it != rq.end())
            druid = std::string(dr_it->value());

        // Sanitize and pop out Ouinet internal HTTP headers.
        auto crq = util::to_cache_request(move(rq));
        if (!crq) {
            yield.log("Invalid request");
            ec = asio::error::invalid_argument;
        }

        // Cache requests do not contain keep-alive information, hence the explicit argument.
        if (!ec) inject_fresh(con, *crq, rq_keep_alive, cancel, yield[ec]);
        return or_throw(yield, ec);
    }

    Connection get_connection(const Request& rq_, Cancel& cancel, YieldContext yield) {
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

    template<class Connection>
    void keep_connection_if(Connection con, bool keep_alive) {
        // NOTE: `con` is put back to `origin_pools` from its destructor unless it
        // is explicitly closed.

        if (!keep_alive)
            con.close();
    }

private:
    AsioExecutor executor;
    asio::ssl::context& ssl_ctx;
    const InjectorConfig& config;
    uuid_generator& genuuid;
    OriginPools& origin_pools;
    string druid{"-"};
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
void handle_request_to_this(Request& rq, GenericStream& con, YieldContext yield)
{
    if (rq.target() == "/api/ok") {
        http::response<http::empty_body> rs{http::status::ok, rq.version()};

        rs.set(http::field::server, OUINET_INJECTOR_SERVER_STRING);
        rs.set(http::field::content_type, "text/plain");
        rs.keep_alive(rq.keep_alive());
        rs.prepare_payload();

        yield.tag("write_res").run([&] (auto y) {
            util::http_reply(con, rs, y);
        });
        return;
    }

    handle_error( con, rq, http::status::not_found, "Unknown injector request"
                , yield.tag("handle_req_error"));
}

//------------------------------------------------------------------------------
static
void serve( const InjectorConfig& config
          , GenericStream con
          , asio::ssl::context& ssl_ctx
          , OriginPools& origin_pools
          , uuid_generator& genuuid
          , Cancel& cancel
          , YieldContext yield_)
{
    auto close_connection_slot = cancel.connect([&con] {
        con.close();
    });

    InjectorCacheControl cc( con.get_executor()
                           , ssl_ctx
                           , origin_pools
                           , config
                           , genuuid);

    auto is_restricted_target = [rx_o = config.target_rx()] (boost::string_view target) {
        if (!rx_o) return false;
        return !boost::regex_match(target.begin(), target.end(), *rx_o);
    };

    // We expect the first request right a way. Consecutive requests may arrive with
    // various delays.
    bool is_first_request = true;
    beast::flat_buffer con_rbuf;  // accumulate reads across iterations here

    uint64_t next_request_id = 0;

    for (;;) {
        sys::error_code ec;
        YieldContext yield = yield_.tag(util::str('R', next_request_id++));

        Request req;
        {
            auto rq_read_timeout = default_timeout::http_recv_simple();
            if (is_first_request) {
                is_first_request = false;
                rq_read_timeout = default_timeout::http_recv_simple_first();
            }

            auto wd = watch_dog(con.get_executor(), rq_read_timeout, [&] { con.close(); });

            yield[ec].tag("read_req").run([&] (auto y) {
                http::async_read(con, con_rbuf, req, y);
            });

            ec = compute_error_code(ec, cancel, wd);
            if (ec) break;
        }

        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done; ec=", ec); });

        bool req_keep_alive = req.keep_alive();

        if (is_request_to_this(req)) {
            handle_request_to_this(req, con, yield[ec].tag("this"));
            if (ec || !req_keep_alive) break;
            continue;
        }

        bool auth = yield[ec].tag("auth").run([&] (auto y) {
                return authenticate(req, con, config.credentials(), y);
        });
        if (!auth) {
            yield.log("Proxy authentication failed");
            if (ec || !req_keep_alive) break;
            continue;
        }
        assert(!ec); ec = {};

        if (req.method() == http::verb::connect) {
            if (!config.is_proxy_enabled()) {
                handle_no_proxy(con, req, yield[ec].tag("proxy/connect/handle_no_proxy"));
                if (ec || !req_keep_alive) break;
                continue;
            }
            return handle_connect_request( move(con), move(con_rbuf)
                                         , req
                                         , cancel  // do not propagate error
                                         , yield[ec].tag("proxy/connect/handle_connect"));
        }

        auto version_hdr_i = req.find(http_::protocol_version_hdr);

        // Check for a Ouinet version header hinting us on
        // whether to behave like an injector or a proxy.
        bool proxy = (version_hdr_i == req.end());

        if (proxy) {
            auto pyield = yield.tag("proxy/plain");

            // No Ouinet header, behave like a (non-caching) proxy.
            if (!config.is_proxy_enabled()) {
                handle_no_proxy(con, req, pyield[ec].tag("handle_no_proxy"));
                if (ec || !req_keep_alive) break;
                continue;
            }

            // TODO: Maybe reject requests for HTTPS URLS:
            // we are perfectly able to handle them (and do verification locally),
            // but the client should be using a CONNECT request instead!
            if (!util::req_ensure_host(req)) {  // origin pools require host
                handle_error( con, req
                            , http::status::bad_request
                            , "Invalid or missing host in request"
                            , pyield[ec].tag("handle_no_host_error"));
                if (ec || !req_keep_alive) break;
                continue;
            }

            pyield.log("BEGIN");

            // Remember to always set `ec` before return in case of error,
            // or the wrong error code will be reported.
            size_t fwd_bytes = 0;
            auto log_result = defer([&] {
                pyield.log("END; ec=", ec, " fwd_bytes=", fwd_bytes);
            });

            auto orig_con = cc.get_connection(req, cancel, pyield[ec].tag("get_connection"));
            if (!ec) {
                auto orig_req = util::to_origin_request(req);
                orig_req.keep_alive(true);  // regardless of what client wants
                pyield[ec].tag("send_request").run([&] (auto y) {
                    util::http_request(orig_con, orig_req, cancel, y);
                });
            }
            bool res_keep_alive = false;
            bool client_was_written_to = false;
            if (!ec) {
                using OrigReader = http_response::Reader;
                Session::reader_uptr rrp = std::make_unique<OrigReader>(move(orig_con));
                auto orig_sess = pyield[ec].tag("read_hdr").run([&] (auto y) {
                    return Session::create(move(rrp), req.method() == http::verb::head, cancel, y);
                });
                if (!ec) {
                    auto& inh = orig_sess.response_header();
                    // Keep proxy connection if the proxy wants to.
                    res_keep_alive = inh.keep_alive();
                    // Keep client connection if the client wants to.
                    inh.keep_alive(req_keep_alive);
                    // Prevent others from inserting ouinet specific header fields.
                    util::remove_ouinet_fields_ref(inh);
                    pyield.log("=== Sending back proxy response ===");
                    pyield.log(inh);

                    pyield[ec].tag("flush").run([&] (auto y) {
                        orig_sess.flush_response(cancel, y, [&] (auto&& part, auto& cc, auto yy) {
                            sys::error_code ee;
                            part.async_write(con, cc, yy[ee]);
                            client_was_written_to = true;  // even with error (possible partial write)
                            return_or_throw_on_error(yy, cc, ee);
                            if (auto b = part.as_body())
                                fwd_bytes += b->size();
                            else if (auto cb = part.as_chunk_body())
                                fwd_bytes += cb->size();
                        }, default_timeout::activity());
                    });
                }
                rrp = orig_sess.release_reader();
                if (rrp)
                    orig_con = ((OrigReader*)(rrp.get()))->release_stream();  // may be reused with keep-alive
                else
                    res_keep_alive = false;
            }
            if (ec) {
                if (!client_was_written_to) {
                    sys::error_code he_ec;
                    handle_error( con, req
                                , http::status::bad_gateway
                                , http_::response_error_hdr_retrieval_failed
                                , "Failed to retrieve content from origin: " + ec.message()
                                , pyield[he_ec].tag("handle_error"));
                }
                if (ec || !req_keep_alive) break;
                continue;
            }

            cc.keep_connection_if(move(orig_con), res_keep_alive);
        }
        else {
            // Ouinet header found, behave like a Ouinet injector.
            auto opt_err_res = util::http_proto_version_error( req, version_hdr_i->value()
                                                             , OUINET_INJECTOR_SERVER_STRING);

            if (opt_err_res) {
                send_response( con, *opt_err_res
                             , yield[ec].tag("inject/write_proto_version_error"));
            } else if (is_restricted_target(req.target())) {
                handle_error( con, req, http::status::forbidden
                            , http_::response_error_hdr_target_not_allowed
                            , "Target not allowed"
                            , yield[ec].tag("inject/handle_restricted"));
            }
            else {
                cc.fetch( con, move(req)
                        , cancel, yield[ec].tag("inject/fetch"));
            }
        }

        if (ec || !req_keep_alive) break;
    }
}

//------------------------------------------------------------------------------
static
void listen( const InjectorConfig& config
           , OuiServiceServer& proxy_server
           , Cancel& cancel
           , YieldContext yield)
{
    uuid_generator genuuid;

    auto stop_proxy_slot = cancel.connect([&proxy_server] {
        proxy_server.stop_listen();
    });

    AsioExecutor exec = proxy_server.get_executor();

    sys::error_code ec;
    proxy_server.start_listen(yield[ec].native());
    if (ec) {
        LOG_ERROR(yield, " Failed to setup ouiservice proxy server; ec=", ec);
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
        GenericStream connection = proxy_server.accept(yield[ec].native());
        if (ec == boost::asio::error::operation_aborted) {
            break;
        } else if (ec) {
            if (!async_sleep(exec, std::chrono::milliseconds(100), cancel, yield.native())) {
                break;
            }
            ec = {};
            continue;
        }

        uint64_t connection_id = next_connection_id++;

        task::spawn_detached(exec, [
            connection = std::move(connection),
            &ssl_ctx,
            &cancel,
            &config,
            &genuuid,
            &origin_pools,
            &yield,
            connection_id,
            lock = shutdown_connections.lock()
        ] (boost::asio::yield_context asio_yield) mutable {
            sys::error_code leaked_ec;
            auto y = YieldContext(asio_yield, yield.log_path().tag(util::str('C', connection_id)));
            serve( config
                 , std::move(connection)
                 , ssl_ctx
                 , origin_pools
                 , genuuid
                 , cancel
                 , y[leaked_ec]);
            if (leaked_ec) {
                // The convention is that `serve` does not throw errors,
                // so complain otherwise but avoid crashing in production.
                LOG_ERROR(y, " Connection serve leaked an error; ec=", leaked_ec);
                assert(0);
            }
        });
    }
}

//------------------------------------------------------------------------------
Injector::Injector(
        InjectorConfig config,
        asio::io_context& ctx,
        util::LogPath log_path,
        std::shared_ptr<bittorrent::MockDht> dht) :
    _config(std::move(config))
{
    #ifndef __WIN32
    if (_config.open_file_limit()) {
        increase_open_file_limit(*_config.open_file_limit());
    }
    #endif

    // Create or load the TLS certificate.
    auto tls_certificate = get_or_gen_tls_cert<EndCertificate>
        ( "localhost"
        , _config.repo_root() / OUINET_TLS_CERT_FILE
        , _config.repo_root() / OUINET_TLS_KEY_FILE
        , _config.repo_root() / OUINET_TLS_DH_FILE );

    AsioExecutor ex = ctx.get_executor();

    if (!_config.is_proxy_enabled())
        LOG_INFO(log_path, "Proxy disabled, not serving plain HTTP/HTTPS proxy requests");
    if (auto target_rx_o = _config.target_rx())
        LOG_INFO(log_path, "Target URIs restricted to regular expression: ", *target_rx_o);
    if (_config.is_private_target_allowed()) {
        LOG_INFO(log_path, "Allowing injection of private targets.");
        g_allow_private_targets = true;
    }
    if (!config.is_doh_enabled()) {
        LOG_INFO("DNS over HTTPS is disabled.");
        g_do_doh = false;
    }

    auto proxy_server = std::make_unique<OuiServiceServer>(ex);

    if (_config.tcp_endpoint()) {
        tcp::endpoint endpoint = *_config.tcp_endpoint();
        LOG_INFO(log_path, " TCP address: ", endpoint);

        util::create_state_file( _config.repo_root()/"endpoint-tcp"
                               , util::str(endpoint));

        proxy_server->add(make_unique<ouiservice::TcpOuiServiceServer>(ex, endpoint));
    }

    _ssl_context = std::make_unique<asio::ssl::context>(
            ssl::util::get_server_context(
                tls_certificate->pem_certificate(),
                tls_certificate->pem_private_key(),
                tls_certificate->pem_dh_param()));

    if (_config.tcp_tls_endpoint()) {
        tcp::endpoint endpoint = *_config.tcp_tls_endpoint();
        LOG_INFO(log_path, " TCP/TLS address: ", endpoint);
        util::create_state_file( _config.repo_root()/"endpoint-tcp-tls"
                               , util::str(endpoint));

        auto base = make_unique<ouiservice::TcpOuiServiceServer>(ex, endpoint);
        proxy_server->add(make_unique<ouiservice::TlsOuiServiceServer>(ex, move(base), *_ssl_context));
    }

    if (_config.utp_endpoint()) {
        udp::endpoint endpoint = *_config.utp_endpoint();
        LOG_INFO(log_path, "uTP address: ", endpoint);

        util::create_state_file( _config.repo_root()/"endpoint-utp"
                               , util::str(endpoint));

        auto srv = make_unique<ouiservice::UtpOuiServiceServer>(ex, endpoint);
        proxy_server->add(move(srv));
    }

    if (_config.utp_tls_endpoint()) {

        udp::endpoint endpoint = *_config.utp_tls_endpoint();

        auto base = make_unique<ouiservice::UtpOuiServiceServer>(ex, endpoint);

        auto local_ep = base->local_endpoint();

        if (local_ep) {
            LOG_INFO(log_path, "uTP/TLS address: ", *local_ep);
            util::create_state_file( _config.repo_root()/"endpoint-utp-tls"
                                   , util::str(*local_ep));
            proxy_server->add(make_unique<ouiservice::TlsOuiServiceServer>(ex, move(base), *_ssl_context));

        } else {
            LOG_ERROR(log_path, " Failed to start uTP/TLS service on ", *_config.utp_tls_endpoint());
        }
    }

    if (dht) {
        _dht = dht;
    } else {
        _dht = std::make_shared<bt::MainlineDht>
            ( ex
            , metrics::Client::noop().mainline_dht()
            , config.is_doh_enabled()
            , fs::path{}
            , _config.bt_bootstrap_extras());  // default storage dir
    }

    _dht->set_endpoints({_config.bittorrent_endpoint()});

    assert(!_dht->local_endpoints().empty());

    if (_dht->local_endpoints().empty())
        LOG_ERROR(log_path, " Failed to bind the BitTorrent DHT to any local endpoint");

    proxy_server->add(make_unique<ouiservice::Bep5Server>
            (_dht, _ssl_context.get(), _config.bep5_injector_swarm_name()));

    #ifdef __EXPERIMENTAL__
    /*
        if (_config.lampshade_endpoint()) {
            tcp::endpoint endpoint = *_config.lampshade_endpoint();
            util::create_state_file( _config.repo_root()/"endpoint-lampshade"
                                   , util::str(endpoint));
    
            unique_ptr<ouiservice::LampshadeOuiServiceServer> server =
                make_unique<ouiservice::LampshadeOuiServiceServer>(ios, endpoint, _config.repo_root()/"lampshade-server");
            LOG_INFO("Lampshade address: ", endpoint, ",key=", server->public_key());
    
            proxy_server->add(std::move(server));
        }
    */

    if (_config.obfs2_endpoint()) {
        tcp::endpoint endpoint = *_config.obfs2_endpoint();
        LOG_INFO("obfs2 address: ", endpoint);
        util::create_state_file( _config.repo_root()/"endpoint-obfs2"
                               , util::str(endpoint));

        proxy_server->add(make_unique<ouiservice::Obfs2OuiServiceServer>(ctx, endpoint, _config.repo_root()/"obfs2-server"));
    }

    if (_config.obfs3_endpoint()) {
        tcp::endpoint endpoint = *_config.obfs3_endpoint();
        LOG_INFO("obfs3 address: ", endpoint);
        util::create_state_file( _config.repo_root()/"endpoint-obfs3"
                               , util::str(endpoint));

        proxy_server->add(make_unique<ouiservice::Obfs3OuiServiceServer>(ctx, endpoint, _config.repo_root()/"obfs3-server"));
    }

    if (_config.obfs4_endpoint()) {
        tcp::endpoint endpoint = *_config.obfs4_endpoint();

        util::create_state_file( _config.repo_root()/"endpoint-obfs4"
                               , util::str(endpoint));

        unique_ptr<ouiservice::Obfs4OuiServiceServer> server =
            make_unique<ouiservice::Obfs4OuiServiceServer>(ctx, endpoint, _config.repo_root()/"obfs4-server");
        task::spawn_detached(ex, [
            obfs4 = server.get(),
            endpoint
        ] (asio::yield_context yield) {
            sys::error_code ec;
            obfs4->wait_for_running(yield[ec]);
            if (!ec) {
                LOG_INFO("obfs4 address: ", endpoint, ",", obfs4->connection_arguments());
            }
        });
        proxy_server->add(std::move(server));
    }

    if (_config.listen_on_i2p()) {
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((_config.repo_root()/"i2p").string(), ex);
        std::unique_ptr<ouiservice::I2pOuiServiceServer> i2p_server = i2p_service->build_server("i2p-private-key");

        auto ep = i2p_server->public_identity();
        LOG_INFO("I2P public ID: ", ep);
        util::create_state_file(_config.repo_root()/"endpoint-i2p", ep);

        proxy_server->add(std::move(i2p_server));
    }
    #endif // ifdef __EXPERIMENTAL__

    LOG_INFO(log_path, " HTTP signing public key (Ed25519): ", _config.cache_private_key().public_key());

    task::spawn_detached(ex, [
        this,
        proxy_server = std::move(proxy_server),
        cancel = _cancel,
        log_path
    ] (asio::yield_context yield) mutable {
        sys::error_code ec;
        listen(_config, *proxy_server, cancel, YieldContext(yield, log_path)[ec]);
    });
}

void Injector::stop() {
    if (_dht) {
        _dht->stop();
        _dht = nullptr;
    }
    _cancel();
    _cancel = Cancel();
}

Injector::~Injector() {
    stop();
}
