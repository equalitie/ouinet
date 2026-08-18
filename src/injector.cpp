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
#include "ouiservice/i2p/service.h"
#include "ouiservice/i2p/session.h"
#include "ouiservice/tcp.h"
#include "ouiservice/utp.h"
#include "ouiservice/tls.h"
#include "ouiservice/bep5/server.h"
#include "ssl/ca_certificate.h"
#include "ssl/util.h"

#include "util/atomic_file.h"
#include "util/bytes.h"
#include "util/file_io.h"
#include "util/spawn_for_result.h"

#include "logger.h"
#include "defer.h"
#include "http_util.h"

#include "cxx/dns.h"
#include "cxx/metrics.h"

namespace ouinet {

using namespace std;

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
using util::AsioExecutor;
template<class V> using SysResult = std::expected<V, sys::error_code>;
using I2pSessionTask = TaskHandle<SysResult<I2pSession>>;

static const fs::path OUINET_TLS_CERT_FILE = "tls-cert.pem";
static const fs::path OUINET_TLS_KEY_FILE = "tls-key.pem";
static const fs::path OUINET_TLS_DH_FILE = "tls-dh.pem";

struct Injector::Inner {
    util::LogPath _log_path;
    std::optional<I2pService> _i2p_service;
    std::optional<I2pSessionTask> _i2p_session_task;

    I2pService* get_or_create_i2p_service(asio::any_io_executor exec, const I2pService::Config& config, Cancel cancel) {
        if (cancel) {
            return nullptr;
        }

        if (_i2p_service) {
            return &*_i2p_service;
        }

        _i2p_service = I2pService::start(config, exec, cancel, _log_path);
        return &*_i2p_service;
    }

    I2pSessionTask get_or_create_i2p_session(asio::any_io_executor exec, const InjectorConfig& config, Cancel cancel) {
        using R = SysResult<I2pSession>;

        if (_i2p_session_task) return *_i2p_session_task;

        auto cfg = config.i2p_service_config();

        _i2p_session_task = spawn_for_result(exec, cancel, _log_path, [this, cfg] (Async yield) -> R {
            if (!cfg) {
                return std::unexpected(asio::error::service_not_found);
            }

            auto service = get_or_create_i2p_service(yield.get_executor(), *cfg, yield.get_cancel());

            if (!service) return std::unexpected(asio::error::service_not_found);

            auto session = _i2p_service->create_session(yield);
            if (!session) return std::unexpected(session.error());

            return std::move(*session);
        });

        return *_i2p_session_task;
    }
};

// TODO: Get rid of this
static bool g_allow_private_targets = false;

//------------------------------------------------------------------------------
template<class Res>
[[nodiscard]]
static
std::expected<void, sys::error_code>
send_response(GenericStream& con, const Res& res, Async yield)
{
    LOG_DEBUG(yield, " === Sending back response ===");
    LOG_DEBUG(yield, " ", res);

    sys::error_code ec = util::http_reply(con, res, yield);
    if (ec) return std::unexpected(ec);
    return {};
}

[[nodiscard]]
static
std::expected<void, sys::error_code>
handle_error( GenericStream& con
            , const Request& req
            , http::status status
            , const string& proto_error
            , const string& message
            , Async yield)
{
    auto res = util::http_error( req.keep_alive(), status
                               , OUINET_INJECTOR_SERVER_STRING, proto_error, message);
    return send_response(con, res, yield);
}

[[nodiscard]]
static
std::expected<void, sys::error_code>
handle_error( GenericStream& con
            , const Request& req
            , http::status status
            , const string& message
            , Async yield)
{
    return handle_error(con, req, status, "", message, yield);
}

[[nodiscard]]
static
std::expected<void, sys::error_code>
handle_no_proxy(GenericStream& con, const Request& req, Async yield)
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
[[nodiscard]]
std::expected<TcpLookup, sys::error_code>
resolve_target( const http::request_header<>& req
              , bool allow_private_targets
              , std::shared_ptr<dns::Resolver> dns_resolver
              , Async yield)
{
    TcpLookup lookup;

    auto host_port = util::get_host_port(req);
    if (!host_port) {
        return std::unexpected(asio::error::invalid_argument);
    }
    auto [host, port] = std::move(*host_port);

    // First test trivial cases (like "localhost" or "127.1.2.3").
    bool local = boost::regex_match(host, util::localhost_rx);
    bool priv = boost::regex_match(host, util::private_addr_rx);

    // Resolve address and also use result for more sophisticaded checking.
    if ((!local && !priv) || allow_private_targets)
    {
        auto r = dns_resolver->resolve(host, port, yield);

        if (r) {
            lookup = std::move(*r);
        }
        else {
            return std::unexpected(r.error());
        }
    }

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

    if ((local || priv) && !allow_private_targets)
    {
        return std::unexpected(asio::error::invalid_argument);
    }

    return lookup;
}

//------------------------------------------------------------------------------
// Note: the connection is attempted towards
// the already resolved endpoints in `lookup`,
// only headers are used from `req`.
//
// `client_c_rbuf` contains data already read from `client_c`
// but not yet processed.
static
std::expected<void, sys::error_code>
handle_connect_request( GenericStream client_c
                      , beast::flat_buffer client_c_rbuf
                      , const Request& req
                      , std::shared_ptr<dns::Resolver> dns_resolver
                      , Async yield)
{
    AsioExecutor exec = yield.get_executor();

    auto disconnect_client_slot = yield.cancel_slot([&client_c] {
        client_c.close();
    });

    auto lookup = resolve_target( req
                                , g_allow_private_targets
                                , std::move(dns_resolver)
                                , yield.tag("resolve"));

    if (!lookup) {
        sys::error_code ec = lookup.error();
        string host;
        auto host_port = util::get_host_port(req);
        if (host_port) {
            host = std::move(host_port->first);
        }

        if (ec == asio::error::invalid_argument) {
            std::ignore = handle_error( client_c, req, http::status::bad_request
                                      , "Illegal target host: " + host
                                      , yield.tag("handle_no_host_error"));
        } else {
            std::ignore = handle_error( client_c, req, http::status::bad_gateway
                                      , http_::response_error_hdr_retrieval_failed
                                      , (ec == asio::error::netdb_errors::host_not_found)
                                        ? ("Could not resolve host: " + host)
                                        : ("Unknown resolver error: " + ec.message())
                                      , yield.tag("handle_resolve_error"));
        }

        return std::unexpected(ec);
    }

    assert(!lookup->empty());

    LOG_DEBUG(yield, " BEGIN");

    // Remember to always set `ec` before return in case of error,
    // or the wrong error code will be reported.
    size_t fwd_bytes_c2o = 0, fwd_bytes_o2c = 0;
    auto log_result = defer([&] {
        LOG_DEBUG(yield, " END; fwd_bytes_c2o=", fwd_bytes_c2o, " fwd_bytes_o2c=", fwd_bytes_o2c);
    });

    auto origin_c = connect_to_host(*lookup, default_timeout::tcp_connect(), yield.tag("connect"));

    if (!origin_c) {
        std::ignore = handle_error( client_c, req
                           , http::status::bad_gateway
                           , http_::response_error_hdr_retrieval_failed
                           , "Failed to connect to origin: " + origin_c.error().message()
                           , yield.tag("handle_connect_error"));

        return std::unexpected(origin_c.error());
    }

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.prepare_payload();

    if (auto r = send_response(client_c, res, yield.tag("write_res")); !r) {
        LOG_DEBUG(yield, " Failed sending CONNECT response; ec=", r.error().message());
        return std::unexpected(r.error());
    }

    // First queue unused but already read data back into the client connnection.
    {
        sys::error_code ec;
        if (client_c_rbuf.size() > 0) client_c.put_back(client_c_rbuf.data(), ec);
        assert(!ec);
    }

    // Forward the rest of data in both directions.
    auto ec = full_duplex(
            std::move(client_c),
            std::move(*origin_c),
            [&] (size_t byte_count) { fwd_bytes_c2o += byte_count; },
            [&] (size_t byte_count) { fwd_bytes_o2c += byte_count; },
            yield.tag("full_duplex"));

    if (ec) return std::unexpected(ec);
    return {};
}

//------------------------------------------------------------------------------
class InjectorCacheControl {
    using Connection = OriginPools::Connection;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code>
    connect(const Request& rq, std::shared_ptr<dns::Resolver> dns_resolver, Async yield)
    {
        try {
            // Parse the URL to tell HTTP/HTTPS, host, port.
            auto url = util::Url::from(rq.target());

            if (!url) {
                LOG_DEBUG(yield, " Unsupported target URL");
                return std::unexpected(asio::error::operation_not_supported);
            }

            // Resolve target endpoint and check its validity.
            auto lookup = resolve_target( rq
                                        , g_allow_private_targets
                                        , std::move(dns_resolver)
                                        , yield);

            if (!lookup) return std::unexpected(lookup.error());

            auto socket = connect_to_host(*lookup, yield);

            if (!socket) return std::unexpected(socket.error());

            if (url->scheme == "https") {
                auto c = ssl::util::client_handshake( std::move(*socket)
                                                    , ssl_ctx
                                                    , url->host
                                                    , yield);

                if (!c) return std::unexpected(c.error());
                return std::move(*c);
            } else {
                return GenericStream(std::move(*socket));
            }
        }
        catch (Async::Cancelled const&) {
            return std::unexpected(asio::error::operation_aborted);
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
        : executor(std::move(executor))
        , ssl_ctx(ssl_ctx)
        , config(config)
        , genuuid(genuuid)
        , origin_pools(origin_pools)
    {
    }

private:
    std::expected<void, sys::error_code>
    inject_fresh( GenericStream& con
                , const Request& cache_rq
                , bool rq_keep_alive
                , shared_ptr<dns::Resolver> dns_resolver
                , Async yield)
    {
        LOG_DEBUG(yield, " BEGIN");

        // Remember to always set before return in case of error,
        // or the wrong error code will be reported.
        size_t fwd_bytes = 0;
        auto log_result = defer([&] {
            LOG_DEBUG(yield, " END; fwd_bytes=", fwd_bytes);
        });

        Session orig_sess;
        try {
            Async timeout_yield = yield;

            // Start a short timeout for initial fetch.
            auto fetch_wd = watch_dog(executor, default_timeout::fetch_http(), [&] { timeout_yield.cancel(); });

            auto orig_con = get_connection(cache_rq, dns_resolver, timeout_yield.tag("connect"));

            if (!orig_con) {
                LOG_DEBUG(yield, " Failed to get connection; ec=", orig_con.error());
                return std::unexpected(orig_con.error());
            }

            // Send HTTP request to origin.
            auto orig_rq = util::to_origin_request(cache_rq);
            orig_rq.keep_alive(true);  // regardless of what client wants
            if (auto r = util::http_request(*orig_con, orig_rq, timeout_yield.tag("request")); !r) {
                LOG_DEBUG(yield, " Failed to send request; ec=", r.error());
                return std::unexpected(r.error());
            }

            Session::reader_uptr sig_reader;
            auto cache_rq_method = cache_rq.method();
            if (cache_rq_method == http::verb::get || cache_rq_method == http::verb::head) {
                auto insert_id = to_string(genuuid());
                auto insert_ts = chrono::seconds(time(nullptr)).count();
                sig_reader = make_unique<cache::SigningReader>
                    (std::move(*orig_con), cache_rq, std::move(insert_id), insert_ts, config.cache_private_key());
            } else {
                // Responses of unsafe or uncacheable requests should not be cached.
                LOG_DEBUG(yield, " Not signing response: not a GET or HEAD request");
                sig_reader = make_unique<http_response::Reader>(std::move(*orig_con));
            }

            auto orig_sess_r = Session::create(
                    std::move(sig_reader),
                    cache_rq_method == http::verb::head,
                    timeout_yield.tag("read_hdr"));

            if (!orig_sess_r) {
                LOG_DEBUG(yield, " Failed to process response head; ec=", orig_sess_r.error());
                return std::unexpected(orig_sess_r.error());
            }

            orig_sess = std::move(*orig_sess_r);
        }
        catch (Async::Cancelled const&) {
            if (yield.is_cancelled()) throw;
            return std::unexpected(asio::error::timed_out);
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

        LOG_DEBUG(yield, " === Sending back injector response ===");
        LOG_DEBUG(yield, " ", orig_sess.response_header());

        auto r = orig_sess.flush_response( yield.tag("flush")
                                         , [&con, &fwd_bytes] (auto&& part, auto y) -> std::expected<void, sys::error_code> {
                auto r = part.async_write(con, y);
                if (!r) return std::unexpected(r.error());
                if (auto b = part.as_body())
                    fwd_bytes += b->size();
                else if (auto cb = part.as_chunk_body())
                    fwd_bytes += cb->size();
                return {};
            }
            , default_timeout::activity());

        if (!r) {
            LOG_DEBUG(yield, " Failed to process response; ec=", r.error());
            return std::unexpected(r.error());
        } else {
            http_logger.log(druid, cache_rq, orig_sess, fwd_bytes);
        }

        keep_connection_if(std::move(orig_sess), rs_keep_alive);

        return {};
    }

public:
    [[nodiscard]]
    std::expected<void, sys::error_code>
    fetch( GenericStream& con
         , Request rq
         , std::shared_ptr<dns::Resolver> dns_resolver
         , Async yield)
    {
        bool rq_keep_alive = rq.keep_alive();

        // Get DRUID before the Ouinet headers are removed.
        auto dr_it = rq.find(http_::request_druid_hdr);
        if (dr_it != rq.end())
            druid = std::string(dr_it->value());

        // Sanitize and pop out Ouinet internal HTTP headers.
        auto crq = util::to_cache_request(std::move(rq));
        if (!crq) {
            LOG_DEBUG(yield, " Invalid request");
            return std::unexpected(asio::error::invalid_argument);
        }

        // Cache requests do not contain keep-alive information, hence the explicit argument.
        return inject_fresh(con, *crq, rq_keep_alive, dns_resolver, yield);
    }

    [[nodiscard]]
    std::expected<Connection, sys::error_code>
    get_connection(const Request& rq_, const std::shared_ptr<dns::Resolver>& dns_resolver, Async yield) {
        Connection connection;

        auto maybe_connection = origin_pools.get_connection(rq_);
        if (maybe_connection) {
            connection = std::move(*maybe_connection);
        } else {
            auto stream = connect(rq_, dns_resolver, yield.tag("connect"));

            if (!stream) return std::unexpected(stream.error());

            connection = origin_pools.wrap(rq_, std::move(*stream));
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
std::expected<void, sys::error_code>
handle_request_to_this(Request& rq, GenericStream& con, Async yield)
{
    if (rq.target() == "/api/ok") {
        http::response<http::empty_body> rs{http::status::ok, rq.version()};

        rs.set(http::field::server, OUINET_INJECTOR_SERVER_STRING);
        rs.set(http::field::content_type, "text/plain");
        rs.keep_alive(rq.keep_alive());
        rs.prepare_payload();

        std::ignore = util::http_reply(con, rs, yield.tag("write_res"));
        return {};
    }

    std::ignore = handle_error( con, rq, http::status::not_found, "Unknown injector request"
                              , yield.tag("handle_req_error"));
    return {};
}

//------------------------------------------------------------------------------
static void serve( InjectorConfig& config
                 , std::shared_ptr<dns::Resolver> dns_resolver
                 , GenericStream con
                 , OriginPools& origin_pools
                 , uuid_generator& genuuid
                 , Async yield_)
{
    auto close_connection_slot = yield_.cancel_slot([&con] {
        con.close();
    });

    InjectorCacheControl cc( con.get_executor()
                           , config.origin_ssl_ctx()
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
        Async yield = yield_.tag(util::str('R', next_request_id++));

        Request req;
        {
            auto rq_read_timeout = default_timeout::http_recv_simple();
            if (is_first_request) {
                is_first_request = false;
                rq_read_timeout = default_timeout::http_recv_simple_first();
            }

            auto wd = watch_dog(con.get_executor(), rq_read_timeout, [&] { con.close(); });

            auto r = http::async_read(con, con_rbuf, req, yield.tag("read_req"));
            if (!r) break;
        }

        LOG_DEBUG(yield, " === New request ===");
        LOG_DEBUG(yield, " ", req.base());
        auto on_exit = defer([&] { LOG_DEBUG(yield, " Done"); });

        bool req_keep_alive = req.keep_alive();

        if (is_request_to_this(req)) {
            auto r = handle_request_to_this(req, con, yield.tag("this"));
            if (!r || !req_keep_alive) break;
            continue;
        }

        if (auto r = authenticate(req, con, config.credentials(), yield.tag("auth")); !r) {
            LOG_DEBUG(yield, " Proxy authentication failed");
            if (!req_keep_alive) break;
            continue;
        }

        if (req.method() == http::verb::connect) {
            if (!config.is_proxy_enabled()) {
                auto r = handle_no_proxy(con, req, yield.tag("proxy/connect/handle_no_proxy"));
                if (!r || !req_keep_alive) break;
                continue;
            }
            std::ignore = handle_connect_request( std::move(con), std::move(con_rbuf)
                                                , req
                                                , dns_resolver
                                                , yield.tag("proxy/connect/handle_connect"));
            return;
        }

        auto version_hdr_i = req.find(http_::protocol_version_hdr);

        // Check for a Ouinet version header hinting us on
        // whether to behave like an injector or a proxy.
        bool proxy = (version_hdr_i == req.end());

        if (proxy) {
            Async pyield = yield.tag("proxy/plain");

            // No Ouinet header, behave like a (non-caching) proxy.
            if (!config.is_proxy_enabled()) {
                auto r = handle_no_proxy(con, req, pyield.tag("handle_no_proxy"));
                if (!r || !req_keep_alive) break;
                continue;
            }

            // TODO: Maybe reject requests for HTTPS URLS:
            // we are perfectly able to handle them (and do verification locally),
            // but the client should be using a CONNECT request instead!
            if (!util::req_ensure_host(req)) {  // origin pools require host
                auto r = handle_error( con, req
                                     , http::status::bad_request
                                     , "Invalid or missing host in request"
                                     , pyield.tag("handle_no_host_error"));
                if (!r || !req_keep_alive) break;
                continue;
            }

            LOG_DEBUG(pyield, " BEGIN");

            // Remember to always set `ec` before return in case of error,
            // or the wrong error code will be reported.
            size_t fwd_bytes = 0;
            auto log_result = defer([&] {
                LOG_DEBUG(pyield, " END; fwd_bytes=", fwd_bytes);
            });

            sys::error_code ec;
            auto orig_con = cc.get_connection(req, dns_resolver, pyield.tag("get_connection"));
            if (!orig_con) ec = orig_con.error();
            if (!ec) {
                auto orig_req = util::to_origin_request(req);
                orig_req.keep_alive(true);  // regardless of what client wants
                auto r = util::http_request(*orig_con, orig_req, pyield.tag("send_request"));
                if (!r) ec = r.error();
            }
            bool res_keep_alive = false;
            bool client_was_written_to = false;
            if (!ec) {
                using OrigReader = http_response::Reader;
                auto orig_sess = Session::create(
                        std::make_unique<OrigReader>(std::move(*orig_con)),
                        req.method() == http::verb::head,
                        yield.tag("read_hdr")
                    );

                if (!orig_sess) ec = orig_sess.error();

                Session::reader_uptr rrp;

                if (!ec) {
                    auto& inh = orig_sess->response_header();
                    // Keep proxy connection if the proxy wants to.
                    res_keep_alive = inh.keep_alive();
                    // Keep client connection if the client wants to.
                    inh.keep_alive(req_keep_alive);
                    // Prevent others from inserting ouinet specific header fields.
                    util::remove_ouinet_fields_ref(inh);
                    LOG_DEBUG(pyield, " === Sending back proxy response ===");
                    LOG_DEBUG(pyield, " ", inh);

                    auto r = orig_sess->flush_response(pyield.tag("flush"),
                                [&] (auto&& part, auto yy) -> std::expected<void, sys::error_code> {
                            auto r = part.async_write(con, yy);
                            client_was_written_to = true;  // even with error (possible partial write)
                            if (!r) return std::unexpected(r.error());
                            if (auto b = part.as_body())
                                fwd_bytes += b->size();
                            else if (auto cb = part.as_chunk_body())
                                fwd_bytes += cb->size();
                            return {};
                        }
                        , default_timeout::activity());

                    if (r) {
                        rrp = orig_sess->release_reader();
                    } else {
                        ec = r.error();
                    }
                }
                if (rrp)
                    orig_con = ((OrigReader*)(rrp.get()))->release_stream();  // may be reused with keep-alive
                else
                    res_keep_alive = false;
            }
            if (ec) {
                if (!client_was_written_to) {
                    std::ignore = handle_error( con, req
                                , http::status::bad_gateway
                                , http_::response_error_hdr_retrieval_failed
                                , "Failed to retrieve content from origin: " + ec.message()
                                , pyield.tag("handle_error"));
                }
                if (!req_keep_alive) break;
                continue;
            }

            cc.keep_connection_if(std::move(*orig_con), res_keep_alive);
        }
        else {
            // Ouinet header found, behave like a Ouinet injector.
            auto opt_err_res = util::http_proto_version_error( req, version_hdr_i->value()
                                                             , OUINET_INJECTOR_SERVER_STRING);

            if (opt_err_res) {
                auto r = send_response( con, *opt_err_res
                             , yield.tag("inject/write_proto_version_error"));
                if (!r) ec = r.error();
            } else if (is_restricted_target(req.target())) {
                auto r = handle_error( con, req, http::status::forbidden
                            , http_::response_error_hdr_target_not_allowed
                            , "Target not allowed"
                            , yield.tag("inject/handle_restricted"));
                if (!r) ec = r.error();
            }
            else {
                auto r = cc.fetch( con, std::move(req)
                        , dns_resolver
                        , yield.tag("inject/fetch"));
                if (!r) ec = r.error();
            }
        }

        if (ec || !req_keep_alive) break;
    }
}

//------------------------------------------------------------------------------
static
void listen( InjectorConfig& config
           , std::shared_ptr<dns::Resolver> dns_resolver
           , OuiServiceServer& proxy_server
           , Async yield)
{
    uuid_generator genuuid;

    auto stop_proxy_slot = yield.cancel_slot([&proxy_server] {
        proxy_server.stop_listen();
    });

    AsioExecutor exec = proxy_server.get_executor();

    sys::error_code ec = proxy_server.start_listen(yield);
    if (ec) {
        LOG_ERROR(yield, " Failed to setup ouiservice proxy server; ec=", ec);
        return;
    }

    WaitCondition shutdown_connections(exec);

    uint64_t next_connection_id = 0;

    OriginPools origin_pools;

    while (true) {
        auto connection = proxy_server.accept(yield);

        if (!connection.has_value()) {
            async_sleep(std::chrono::milliseconds(100), yield);
            continue;
        }

        uint64_t connection_id = next_connection_id++;

        yield.spawn([
            connection = std::move(*connection),
            &config,
            &dns_resolver,
            &genuuid,
            &origin_pools,
            connection_id,
            lock = shutdown_connections.lock()
        ] (Async yield) mutable {
            serve( config
                 , dns_resolver
                 , std::move(connection)
                 , origin_pools
                 , genuuid
                 , yield.tag(util::str('C', connection_id)));
        });
    }
}

//------------------------------------------------------------------------------
Injector::Injector(
        InjectorConfig config,
        asio::io_context& ctx,
        util::LogPath log_path,
        std::shared_ptr<bittorrent::MockDht> mock_dht) :
    _exec(ctx.get_executor()),
    _config(std::move(config)),
    _dns_resolver(std::make_shared<dns::Resolver>(_config.dns_config())),
    _inner(std::make_unique<Inner>(log_path))
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

    if (!_config.is_proxy_enabled())
        LOG_INFO(log_path, " Proxy disabled, not serving plain HTTP/HTTPS proxy requests");
    if (auto target_rx_o = _config.target_rx())
        LOG_INFO(log_path, " Target URIs restricted to regular expression: ", *target_rx_o);
    if (_config.is_private_target_allowed()) {
        LOG_INFO(log_path, " Allowing injection of private targets.");
        g_allow_private_targets = true;
    }
    LOG_INFO( "DNS protocols enabled: ["
            , dns::Resolver::protos_to_str(_config.dns_config().protocols)
            , "].");

    auto proxy_server = std::make_unique<OuiServiceServer>(_exec);

    if (_config.tcp_endpoint()) {
        tcp::endpoint endpoint = *_config.tcp_endpoint();
        LOG_INFO(log_path, " TCP address: ", endpoint);

        util::create_state_file( _config.repo_root()/"endpoint-tcp"
                               , util::str(endpoint));

        proxy_server->add(make_unique<ouiservice::TcpOuiServiceServer>(_exec, endpoint));
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

        auto base = make_unique<ouiservice::TcpOuiServiceServer>(_exec, endpoint);
        proxy_server->add(make_unique<ouiservice::TlsOuiServiceServer>(_exec, std::move(base), *_ssl_context));
    }

    if (_config.utp_endpoint()) {
        udp::endpoint endpoint = *_config.utp_endpoint();
        LOG_INFO(log_path, " uTP address: ", endpoint);

        util::create_state_file( _config.repo_root()/"endpoint-utp"
                               , util::str(endpoint));

        auto srv = make_unique<ouiservice::UtpOuiServiceServer>(_exec, endpoint, log_path);
        proxy_server->add(std::move(srv));
    }

    if (_config.utp_tls_endpoint()) {

        udp::endpoint endpoint = *_config.utp_tls_endpoint();

        auto base = make_unique<ouiservice::UtpOuiServiceServer>(_exec, endpoint, log_path);

        auto local_ep = base->local_endpoint();

        if (local_ep) {
            LOG_INFO(log_path, " uTP/TLS address: ", *local_ep);
            util::create_state_file( _config.repo_root()/"endpoint-utp-tls"
                                   , util::str(*local_ep));
            proxy_server->add(make_unique<ouiservice::TlsOuiServiceServer>(_exec, std::move(base), *_ssl_context));

        } else {
            LOG_ERROR(log_path, " Failed to start uTP/TLS service on ", *_config.utp_tls_endpoint());
        }
    }

    if (mock_dht) {
        _dht = mock_dht;
    } else {
        auto dht = std::make_shared<bt::MainlineDht>(
            _exec,
            metrics::Client::noop().mainline_dht(),
            _dns_resolver,
            config.udp_mux_rx_limit_in_bytes(),
            fs::path{},  // default storage dir
            bt::bootstrap::Config()
                .with_default(!_config.bt_bootstrap_no_default())
                .with_extras(_config.bt_bootstrap_extras()),
            log_path.tag("dht")
        );

        if (_config.bt_allow_martians()) {
            dht->set_peer_filter(bt::PeerFilter::none);
        }

        _dht = std::move(dht);
    }

    _dht->set_endpoints({_config.bittorrent_endpoint()});

    assert(!_dht->local_endpoints().empty());

    if (_dht->local_endpoints().empty())
        LOG_ERROR(log_path, " Failed to bind the BitTorrent DHT to any local endpoint");

    proxy_server->add(make_unique<ouiservice::Bep5Server>(
        _dht,
        _ssl_context.get(),
        _config.bep5_injector_swarm_name(),
        log_path
    ));

    if (_config.listen_on_i2p()) {
        struct Server : public OuiServiceImplementationServer {
            sys::error_code start_listen(Async) override {
                return sys::error_code();
            }

            void stop_listen() override { _cancel(); }

            std::expected<GenericStream, sys::error_code> accept(Async yield) override {
                auto& s = _session_task.wait_ref(yield);

                if (!s) {
                    LOG_WARN(_log_path, " I2P session was not created");
                    return std::unexpected(s.error());
                }

                auto result = s->accept(yield);

                if (!result.has_value()) {
                    LOG_WARN(_log_path, " Failed to accept I2P connection");
                    return std::unexpected(result.error());
                }

                return std::move(*result);
            }

            Server(I2pSessionTask session_task, util::LogPath log_path):
                _session_task(std::move(session_task)),
                _log_path(std::move(log_path))
            {}

            I2pSessionTask _session_task;
            LifetimeCancel _cancel;
            util::LogPath _log_path;
        };

        proxy_server->add(std::make_unique<Server>(
            _inner->get_or_create_i2p_session(_exec, _config, _cancel),
            log_path
        ));
    }

    LOG_INFO(log_path, " HTTP signing public key (Ed25519): ", _config.cache_private_key().public_key());

    task::spawn_detached(_exec, [
        this,
        proxy_server = std::move(proxy_server),
        cancel = _cancel,
        log_path
    ] (asio::yield_context yield) mutable {
        listen(_config, _dns_resolver, *proxy_server, Async(yield, cancel, log_path));
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

std::expected<I2pAddress, sys::error_code> Injector::i2p_address(Async yield) {
    auto session_task = _inner->get_or_create_i2p_session(_exec, _config, _cancel);

    auto& result = session_task.wait_ref(yield);

    if (!result) return std::unexpected(result.error());

    return result->local_addr();
}

} // namespace ouinet
