#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/format.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/optional/optional_io.hpp>
#include <iostream>
#include <cstdlib>  // for atexit()

#include "cache/cache_client.h"
#include "cache/http_desc.h"

#include "namespaces.h"
#include "origin_pools.h"
#include "http_util.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_stream.h"
#include "util.h"
#include "async_sleep.h"
#include "endpoint.h"
#include "cache_control.h"
#include "or_throw.h"
#include "request_routing.h"
#include "full_duplex_forward.h"
#include "client_config.h"
#include "client.h"
#include "authenticate.h"
#include "defer.h"
#include "default_timeout.h"
#include "constants.h"
#include "ssl/ca_certificate.h"
#include "ssl/dummy_certificate.h"
#include "ssl/util.h"

#ifndef __ANDROID__
#  include "force_exit_on_signal.h"
#endif // ifndef __ANDROID__

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/pt-obfs2.h"
#include "ouiservice/pt-obfs3.h"
#include "ouiservice/pt-obfs4.h"
#include "ouiservice/tcp.h"
#include "ouiservice/tls.h"

#include "util/signal.h"
#include "util/crypto.h"
#include "util/lru_cache.h"
#include "util/scheduler.h"

#include "logger.h"

using namespace std;
using namespace ouinet;

namespace posix_time = boost::posix_time;

using tcp      = asio::ip::tcp;
using Request  = http::request<http::string_body>;
using Response = http::response<http::dynamic_body>;
using boost::optional;

static const fs::path OUINET_CA_CERT_FILE = "ssl-ca-cert.pem";
static const fs::path OUINET_CA_KEY_FILE = "ssl-ca-key.pem";
static const fs::path OUINET_CA_DH_FILE = "ssl-ca-dh.pem";

//------------------------------------------------------------------------------
class Client::State : public enable_shared_from_this<Client::State> {
    friend class Client;

public:
    State(asio::io_service& ios, ClientConfig cfg)
        : _ios(ios)
        , _config(move(cfg))
        // A certificate chain with OUINET_CA + SUBJECT_CERT
        // can be around 2 KiB, so this would be around 2 MiB.
        // TODO: Fine tune if necessary.
        , _ssl_certificate_cache(1000)
        , ssl_ctx{asio::ssl::context::tls_client}
        , inj_ctx{asio::ssl::context::tls_client}
        , _fetch_stored_scheduler(_ios, 1)
    {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer);

        // We do *not* want to do this since
        // we will not be checking certificate names,
        // thus any certificate signed by a recognized CA
        // would be accepted if presented by an injector.
        //inj_ctx.set_default_verify_paths();
        inj_ctx.set_verify_mode(asio::ssl::verify_peer);
    }

    void start();

    void stop() {
        _cache = nullptr;
        _shutdown_signal();
        if (_injector) _injector->stop();
    }

    void setup_ipfs_cache();
    void set_injector(string);

private:
    GenericStream ssl_mitm_handshake( GenericStream&&
                                    , const Request&
                                    , asio::yield_context);

    void serve_request(GenericStream&& con, asio::yield_context yield);

    CacheEntry
    fetch_stored( const Request& request
                , request_route::Config& request_config
                , Cancel& cancel
                , Yield yield);

    Response fetch_fresh( const Request&
                        , request_route::Config&
                        , bool& out_can_store
                        , Cancel& cancel
                        , Yield);

    Response fetch_fresh_from_origin(const Request&, Cancel&, Yield);

    Response fetch_fresh_through_connect_proxy(const Request&, Cancel&, Yield);

    CacheControl build_cache_control(request_route::Config& request_config);

    void listen_tcp( asio::yield_context
                   , tcp::endpoint
                   , function<void(GenericStream, asio::yield_context)>);

    void setup_injector(asio::yield_context);

    bool was_stopped() const {
        return _shutdown_signal.call_count() != 0;
    }

    fs::path ca_cert_path() const { return _config.repo_root() / OUINET_CA_CERT_FILE; }
    fs::path ca_key_path()  const { return _config.repo_root() / OUINET_CA_KEY_FILE;  }
    fs::path ca_dh_path()   const { return _config.repo_root() / OUINET_CA_DH_FILE;   }

    asio::io_service& get_io_service() { return _ios; }
    Signal<void()>& get_shutdown_signal() { return _shutdown_signal; }

    bool maybe_handle_websocket_upgrade( GenericStream&
                                       , beast::string_view connect_host_port
                                       , Request&
                                       , Yield);

    GenericStream connect_to_origin(const Request&, Cancel&, Yield);

private:
    asio::io_service& _ios;
    ClientConfig _config;
    std::unique_ptr<CACertificate> _ca_certificate;
    util::LruCache<string, string> _ssl_certificate_cache;
    std::unique_ptr<OuiServiceClient> _injector;
    std::unique_ptr<CacheClient> _cache;

    ClientFrontEnd _front_end;
    Signal<void()> _shutdown_signal;

    bool _is_ipns_being_setup = false;

    // For debugging
    uint64_t _next_connection_id = 0;
    ConnectionPool<std::string> _injector_connections;
    OriginPools _origin_pools;

    asio::ssl::context ssl_ctx;
    asio::ssl::context inj_ctx;

    Scheduler _fetch_stored_scheduler;
};

//------------------------------------------------------------------------------
static
void handle_bad_request( GenericStream& con
                       , const Request& req
                       , string message
                       , Yield yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, OUINET_CLIENT_SERVER_STRING);
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
CacheEntry
Client::State::fetch_stored( const Request& request
                           , request_route::Config& request_config
                           , Cancel& cancel
                           , Yield yield)
{
    const bool cache_is_disabled
        = !request_config.enable_stored
       || !_cache
       || !_front_end.is_ipfs_cache_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheEntry>( yield
                                   , asio::error::operation_not_supported);
    }

    sys::error_code ec;
    auto ret = _cache->get_content( key_from_http_req(request)
                                  , _config.default_index_type()
                                  , cancel
                                  , yield[ec]);
    if (!ec) {
        // Prevent others from inserting ouinet headers.
        ret.second.response = util::remove_ouinet_fields(move(ret.second.response));

        // Add an injection identifier header
        // to enable the user to track injection state.
        ret.second.response.set(http_::response_injection_id_hdr, ret.first);
    }

    return or_throw(yield, ec, move(ret.second));
}

//------------------------------------------------------------------------------
GenericStream
Client::State::connect_to_origin( const Request& rq
                                , Cancel& cancel
                                , Yield yield)
{
    std::string host, port;
    std::tie(host, port) = util::get_host_port(rq);

    sys::error_code ec;

    auto lookup = util::tcp_async_resolve( host, port
                                         , _ios
                                         , cancel
                                         , yield[ec]);

    if (ec) return or_throw<GenericStream>(yield, ec);

    auto sock = connect_to_host(lookup, _ios, cancel, yield[ec]);

    if (ec) return or_throw<GenericStream>(yield, ec);

    GenericStream stream;

    if (rq.target().starts_with("https:") || rq.target().starts_with("wss:")) {
        stream = ssl::util::client_handshake( move(sock)
                                            , ssl_ctx
                                            , host
                                            , cancel
                                            , yield[ec]);

        if (ec) return or_throw(yield, ec, move(stream));
    }
    else {
        stream = move(sock);
    }

    return stream;
}
//------------------------------------------------------------------------------

Response Client::State::fetch_fresh_from_origin( const Request& rq
                                               , Cancel& cancel_
                                               , Yield yield)
{
    Cancel cancel(cancel_);

    WatchDog watch_dog(_ios
                      , default_timeout::fetch_http()
                      , [&] { cancel(); });

    auto con = _origin_pools.get_connection(rq);

    sys::error_code ec;

    if (!con) {
        auto stream = connect_to_origin(rq, cancel, yield[ec]);

        if (!ec && cancel) ec = asio::error::timed_out;
        if (ec) return or_throw<Response>(yield, ec);

        con.reset(new ConnectionPool<>::Connection(move(stream), boost::none));
    }

    // Transform request from absolute-form to origin-form
    // https://tools.ietf.org/html/rfc7230#section-5.3
    auto rq_ = util::req_form_from_absolute_to_origin(rq);

    auto res = con->request(rq_, cancel, yield[ec]);

    if (!ec && res.keep_alive()) {
        _origin_pools.insert_connection(rq, move(con));
    }

    return or_throw(yield, ec, move(res));
}

//------------------------------------------------------------------------------
Response Client::State::fetch_fresh_through_connect_proxy( const Request& rq
                                                         , Cancel& cancel_
                                                         , Yield yield)
{
    Cancel cancel(cancel_);
    WatchDog watch_dog(_ios, default_timeout::fetch_http(), [&]{ cancel(); });

    // Parse the URL to tell HTTP/HTTPS, host, port.
    util::url_match url;

    if (!match_http_url(rq.target(), url)) {
        // unsupported URL
        return or_throw<Response>(yield, asio::error::operation_not_supported);
    }

    // Connect to the injector/proxy.
    sys::error_code ec;
    auto inj = _injector->connect(yield[ec].tag("connect_to_injector"), cancel);

    if (ec) return or_throw<Response>(yield, ec);

    // Build the actual request to send to the proxy.
    Request connreq = { http::verb::connect
                      , url.host + ":" + (url.port.empty() ? "443" : url.port)
                      , 11 /* HTTP/1.1 */};

    // HTTP/1.1 requires a ``Host:`` header in all requests:
    // <https://tools.ietf.org/html/rfc7230#section-5.4>.
    connreq.set(http::field::host, connreq.target());

    if (auto credentials = _config.credentials_for(inj.remote_endpoint))
        connreq = authorize(connreq, *credentials);

    // Open a tunnel to the origin
    // (to later perform the SSL handshake and send the request).
    // Only get the head of the CONNECT response
    // (otherwise we would get stuck waiting to read
    // a body whose length we do not know
    // since the respone should have no content length).
    auto connres = fetch_http<http::empty_body>
                        ( _ios
                        , inj.connection
                        , connreq
                        , default_timeout::fetch_http()
                        , cancel
                        , yield[ec].tag("connreq"));

    if (connres.result() != http::status::ok) {
        // This error code is quite fake, so log the error too.
        // Unfortunately there is no body to show.
        yield.tag("proxy_connect").log(connres);
        return or_throw<Response>(yield, asio::error::connection_refused);
    }

    GenericStream con;

    if (url.scheme == "https") {
        con = ssl::util::client_handshake( move(inj.connection)
                                         , ssl_ctx
                                         , url.host
                                         , cancel
                                         , yield[ec]);
    } else {
        con = move(inj.connection);
    }

    if (ec) return or_throw<Response>(yield, ec);

    // TODO: move
    auto rq_ = util::req_form_from_absolute_to_origin(rq);

    auto res = fetch_http<http::dynamic_body>(_ios, con, rq_, cancel, yield[ec]);

    if (ec) return or_throw(yield, ec, move(res));

    // Prevent others from inserting ouinet headers.
    return util::remove_ouinet_fields(move(res));
}
//------------------------------------------------------------------------------
Response Client::State::fetch_fresh
        ( const Request& request
        , request_route::Config& request_config
        , bool& out_can_store
        , Cancel& cancel
        , Yield yield)
{
    using namespace asio::error;
    using request_route::fresh_channel;

    // TODO: This probably isn't necessary because cancel()
    // is (should be?) called from above.
    auto shutdown_slot = _shutdown_signal.connect([&] { cancel(); });

    out_can_store = false;

    sys::error_code last_error = operation_not_supported;

    LOG_DEBUG("fetching fresh");

    while (!request_config.fresh_channels.empty()) {
        auto r = request_config.fresh_channels.front();
        request_config.fresh_channels.pop();

        switch (r) {
            case fresh_channel::origin: {
                if (!_config.is_origin_access_enabled()) {
                    continue;
                }

                sys::error_code ec;
                Response res = fetch_fresh_from_origin( request
                                                      , cancel
                                                      , yield[ec]);

                if (ec) {
                    last_error = ec;
                    continue;
                }

                // Prevent others from inserting ouinet headers.
                return util::remove_ouinet_fields(move(res));
            }
            // Since the current implementation uses the injector as a proxy,
            // both cases are quite similar, so we only handle HTTPS requests here.
            case fresh_channel::proxy: {
                if (!_config.is_proxy_access_enabled())
                    continue;

                if (request.target().starts_with("https://")) {
                    sys::error_code ec;

                    auto res = fetch_fresh_through_connect_proxy( request
                                                                , cancel
                                                                , yield[ec]);

                    if (ec) {
                        last_error = ec;
                        continue;
                    }

                    return res;
                }
            }
            // Fall through, the case below handles both injector and proxy with plain HTTP.
            case fresh_channel::injector: {
                if (r == fresh_channel::injector && !_front_end.is_injector_proxying_enabled())
                    continue;

                // Connect to the injector.
                sys::error_code ec;

                using Con = ConnectionPool<std::string>::Connection;

                unique_ptr<Con> con = _injector_connections.pop_front();

                if (!con) {
                    auto c = _injector->connect
                        (yield[ec].tag("connect_to_injector2"), cancel);

                    if (ec) {
                        last_error = ec;
                        continue;
                    }

                    con = make_unique<Con>( move(c.connection)
                                          , move(c.remote_endpoint) );
                }

                // Build the actual request to send to the injector.
                Request injreq = request;
                if (auto credentials = _config.credentials_for(con->aux))
                    injreq = authorize(injreq, *credentials);
                if (r == fresh_channel::injector)
                    injreq = util::to_injector_request(move(injreq));
                injreq.keep_alive(request.keep_alive());

                // Send the request to the injector/proxy.
                auto res = con->request( injreq
                                       , cancel
                                       , yield[ec].tag("inj-request"));
                if (ec) {
                    last_error = ec;
                    continue;
                }

                out_can_store = (r == fresh_channel::injector);

                if (res.keep_alive()) {
                    _injector_connections.push_back(std::move(con));
                }

                return res;
            }
            case fresh_channel::_front_end: {
                sys::error_code ec;

                auto res = _front_end.serve( _config
                                           , request
                                           , _cache.get()
                                           , *_ca_certificate
                                           , yield[ec].tag("serve_frontend"));
                if (ec) {
                    last_error = ec;
                    continue;
                }

                return res;
            }
        }
    }

    return or_throw<Response>(yield, last_error);
}

//------------------------------------------------------------------------------
class Client::ClientCacheControl {
public:
    ClientCacheControl( Client::State& client_state
                      , request_route::Config& request_config)
        : client_state(client_state)
        , request_config(request_config)
        , cc(client_state.get_io_service(), OUINET_CLIENT_SERVER_STRING)
    {
        cc.fetch_fresh = [&] (const Request& rq, Cancel& cancel, Yield yield) {
            return fetch_fresh(rq, cancel, yield);
        };

        cc.fetch_stored = [&] (const Request& rq, Cancel& cancel, Yield yield) {
            return fetch_stored(rq, cancel, yield);
        };

        cc.store = [&](const Request& rq, Response rs, Cancel& cancel, Yield yield) {
            return store(rq, move(rs), cancel, yield);
        };

        cc.max_cached_age(client_state._config.max_cached_age());
    }

    Response fetch_fresh(const Request& request, Cancel& cancel, Yield yield) {
        sys::error_code ec;
        auto r = client_state.fetch_fresh( request
                                         , request_config
                                         , _can_store
                                         , cancel
                                         , yield[ec]);

        if (!ec) {
            yield.log("Fetched fresh success, status: ", r.result());
        } else {
            yield.log("Fetched fresh error: ", ec.message());
        }

        return or_throw(yield, ec, move(r));
    }

    CacheEntry
    fetch_stored(const Request& request, Cancel& cancel, Yield yield) {
        yield.log("Fetching from cache");

        sys::error_code ec;
        auto r = client_state.fetch_stored( request
                                          , request_config
                                          , cancel
                                          , yield[ec]);

        if (!ec) {
            yield.log("Fetched from cache success, status: ", r.response.result());
        } else {
            yield.log("Fetched from cache error: ", ec.message());
        }

        return or_throw(yield, ec, move(r));
    }

    Response store(const Request& rq, Response rs, Cancel&, Yield yield)
    {
        // No need to filter request or response headers
        // since we are not storing them here
        // (they can be found at the descriptor).
        // Otherwise we should pass them through
        // `util::to_cache_request` and `util::to_cache_response` (respectively).

        sys::error_code ec;

        auto& cache = client_state._cache;

        if (!_can_store) ec = asio::error::invalid_argument;
        if (!cache)      ec = asio::error::operation_not_supported;

        if (ec) return or_throw(yield, ec, move(rs));

        asio::spawn(client_state.get_io_service(),
            [ &cache, rs
            , &ios = client_state.get_io_service()
            , &cancel = client_state.get_shutdown_signal()
            , key = key_from_http_req(rq)
            , indextype = client_state._config.default_index_type()
            ] (asio::yield_context yield) mutable {
                // Seed content data itself.
                // TODO: Use the scheduler here to only do some max number
                // of `ipfs_add`s at a time. Also then trim that queue so
                // that it doesn't grow indefinitely.
                sys::error_code ec;
                auto body_link = cache->ipfs_add
                    (beast::buffers_to_string(rs.body().data()), yield[ec]);

                auto inj_id = rs[http_::response_injection_id_hdr].to_string();
                rs = Response();  // drop heavy response body

                static const int max_attempts = 3;
                auto log_post_inject =
                    [&] (int attempt, const string& msg){
                        // Adjust attempt number for meaningful 1-based logging.
                        attempt = (attempt < max_attempts) ? attempt + 1 : max_attempts;
                        LOG_DEBUG( "Post-inject lookup id=", inj_id
                                 , " (", attempt, "/", max_attempts, "): "
                                 , msg, "; key=", key);
                    };

                // Retrieve the descriptor for the injection that we triggered
                // so that we help seed the URL->descriptor mapping too.
                // Try a few times to get the descriptor
                // (after some insertion delay, with exponential backoff).
                optional<Descriptor> desc;
                int attempt = 0;
                for ( auto backoff = chrono::seconds(30);
                      attempt < max_attempts; backoff *= 2, ++attempt) {
                    if (!async_sleep(ios, backoff, cancel, yield))
                        return;

                    sys::error_code ec;
                    auto desc_data = cache->get_descriptor(key, indextype, cancel, yield[ec]);
                    if (ec == asio::error::not_found) {  // not (yet) inserted
                        log_post_inject(attempt, "not found, try again");
                        continue;
                    } else if (ec) {  // some other error
                        log_post_inject(attempt, (boost::format("error=%s, giving up") % ec).str());
                        return;
                    }

                    desc = Descriptor::deserialize(desc_data);
                    if (!desc) {  // maybe incompatible cache index
                        log_post_inject(attempt, "invalid descriptor, giving up");
                        return;
                    }

                    if (inj_id == desc->request_id)
                        break;  // found desired descriptor

                    // different injection, try again
                }

                log_post_inject
                    (attempt, desc ? ( boost::format("same_desc=%b same_data=%b")
                                     % (inj_id == desc->request_id)
                                     % (body_link == desc->body_link)).str()
                                   : "did not find descriptor, giving up");
            });

        // Note: we have to return a valid response even in case of error
        // because CacheControl will use it.
        return or_throw(yield, ec, move(rs));
    }

    Response fetch(const Request& rq, Yield yield)
    {
        Cancel cancel;

        bool timed_out = false;

        WatchDog wd( client_state.get_io_service()
                   , chrono::seconds(3*60)
                   , [&] {
                         timed_out = true;
                         cancel();
                   });

        sys::error_code ec;
        Response rs = cc.fetch(rq, cancel, yield[ec]);

        if (timed_out) ec = asio::error::timed_out;

        return or_throw(yield, ec, move(rs));
    }

private:
    Client::State& client_state;
    request_route::Config& request_config;
    bool _can_store;
    CacheControl cc;
};

//------------------------------------------------------------------------------
//static
//Response bad_gateway(const Request& req)
//{
//    Response res{http::status::bad_gateway, req.version()};
//    res.set(http::field::server, "Ouinet");
//    res.keep_alive(req.keep_alive());
//    return res;
//}

//------------------------------------------------------------------------------
static
string base_domain_from_target(const beast::string_view& target)
{
    auto full_host = target.substr(0, target.rfind(':'));
    size_t dot0, dot1 = 0;
    if ((dot0 = full_host.find('.')) != full_host.rfind('.'))
        // Two different dots were found
        // (e.g. "www.example.com" but not "localhost" or "example.com").
        dot1 = dot0 + 1;  // skip first component and dot (e.g. "www.")
    return full_host.substr(dot1).to_string();
}

//------------------------------------------------------------------------------
GenericStream Client::State::ssl_mitm_handshake( GenericStream&& con
                                               , const Request& con_req
                                               , asio::yield_context yield)
{
    // TODO: We really should be waiting for
    // the TLS Client Hello message to arrive at the clear text connection
    // (after we send back 200 OK),
    // then retrieve the value of the Server Name Indication (SNI) field
    // and rewind the Hello message,
    // but for the moment we will assume that the browser sends
    // a host name instead of an IP address or its reverse resolution.
    auto base_domain = base_domain_from_target(con_req.target());

    const string* crt_chain = _ssl_certificate_cache.get(base_domain);

    if (!crt_chain) {
        DummyCertificate dummy_crt(*_ca_certificate, base_domain);

        crt_chain
            = _ssl_certificate_cache.put(move(base_domain)
                                        , dummy_crt.pem_certificate()
                                          + _ca_certificate->pem_certificate());
    }

    auto ssl_context = ssl::util::get_server_context
        ( *crt_chain
        , _ca_certificate->pem_private_key()
        , _ca_certificate->pem_dh_param());

    // Send back OK to let the UA know we have the "tunnel"
    http::response<http::string_body> res{http::status::ok, con_req.version()};
    http::async_write(con, res, yield);

    sys::error_code ec;

    auto ssl_sock = make_unique<asio::ssl::stream<GenericStream>>(move(con), ssl_context);
    ssl_sock->async_handshake(asio::ssl::stream_base::server, yield[ec]);
    if (ec) return or_throw<GenericStream>(yield, ec);

    static const auto ssl_shutter = [](asio::ssl::stream<GenericStream>& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericStream(move(ssl_sock), move(ssl_shutter));
}

//------------------------------------------------------------------------------
// Return true if res indicated an error from the injector
bool handle_if_injector_error(GenericStream& con, Response& res_, Yield yield) {
    auto err_hdr_i = res_.find(http_::response_error_hdr);

    if (err_hdr_i == res_.end()) return false; // No error

    Response res{http::status::bad_request, 11};
    res.set(http::field::server, OUINET_CLIENT_SERVER_STRING);
    res.set(http_::response_error_hdr, err_hdr_i->value());
    res.keep_alive(false);

    string body = "Incompatible Ouinet request version";

    Response::body_type::reader reader(res, res.body());
    sys::error_code ec;
    reader.put(asio::buffer(body), ec);
    assert(!ec);

    res.prepare_payload();

    http::async_write(con, res, yield[ec]);

    return true;
}

//------------------------------------------------------------------------------
bool Client::State::maybe_handle_websocket_upgrade( GenericStream& browser
                                                  , beast::string_view connect_hp
                                                  , Request& rq
                                                  , Yield yield)
{
    sys::error_code ec;

    if (!boost::iequals(rq[http::field::upgrade], "websocket"))  return false;

    bool has_upgrade = false;

    for (auto s : SplitString(rq[http::field::connection], ',')) {
        if (boost::iequals(s, "Upgrade")) { has_upgrade = true; break; }
    }

    if (!has_upgrade) return false;

    if (!rq.target().starts_with("ws:") && !rq.target().starts_with("wss:")) {
        if (connect_hp.empty()) {
            handle_bad_request(browser, rq, "Not a websocket server", yield[ec]);
            return true;
        }

        // Make this a "proxy" request. Among other things, this is important
        // to let the consecutive code know we want encryption.
        rq.target( string("wss://")
                 + ( (rq[http::field::host].length() > 0)
                     ? rq[http::field::host]
                     : connect_hp).to_string()
                 + rq.target().to_string());
    }

    Cancel cancel(_shutdown_signal);

    // TODO: Reuse existing connections to origin and injectors.  Currently
    // this is hard because those are stored not as streams but as
    // ConnectionPool::Connection.
    auto origin = connect_to_origin(rq, cancel, yield[ec]);

    if (ec) return or_throw(yield, ec, true);

    http::async_write(origin, rq, yield[ec]);

    beast::flat_buffer buffer;
    Response rs;
    http::async_read(origin, buffer, rs, yield[ec]);

    if (ec) return or_throw(yield, ec, true);

    http::async_write(browser, rs, yield[ec]);

    if (rs.result() != http::status::switching_protocols) return true;

    full_duplex(move(browser), move(origin), yield[ec]);

    return or_throw(yield, ec, true);
}

//------------------------------------------------------------------------------
void Client::State::serve_request( GenericStream&& con
                                 , asio::yield_context yield_)
{
    LOG_DEBUG("Request received ");

    namespace rr = request_route;
    using rr::fresh_channel;

    auto close_con_slot = _shutdown_signal.connect([&con] {
        con.close();
    });

    // This request router configuration will be used for requests by default.
    //
    // Looking up the cache when needed is allowed, while for fetching fresh
    // content:
    //
    //  - the origin is first contacted directly,
    //    for good overall speed and responsiveness
    //  - if not available, the injector is used to
    //    get the content and cache it for future accesses
    //  - otherwise the content is fetched via the proxy
    //
    // So enabling the Injector channel will result in caching content
    // when access to the origin is not possible,
    // while disabling the Injector channel will resort to the proxy
    // when access to the origin is not possible,
    // but it will keep the browsing private and not cache anything.
    //
    // To also avoid getting content from the cache
    // (so that browsing looks like using a normal non-caching proxy)
    // the cache can be disabled.
    const rr::Config default_request_config
        { true
        , queue<fresh_channel>({ fresh_channel::origin
                               , fresh_channel::injector
                               , fresh_channel::proxy})};

    // This is the matching configuration for the one above,
    // but for uncacheable requests.
    const rr::Config nocache_request_config
        { false
        , queue<fresh_channel>({ fresh_channel::origin
                               , fresh_channel::proxy})};

    // The currently effective request router configuration.
    rr::Config request_config;

    Client::ClientCacheControl cache_control(*this, request_config);

    sys::error_code ec;
    beast::flat_buffer buffer;

    // Expressions to test the request against and configurations to be used.
    // TODO: Create once and reuse.
    using Match = pair<const ouinet::reqexpr::reqex, const rr::Config>;

    auto method_getter([](const Request& r) {return r.method_string();});
    auto host_getter([](const Request& r) {return r["Host"];});
    auto x_oui_dest_getter([](const Request& r) {return r["X-Oui-Destination"];});
    auto target_getter([](const Request& r) {return r.target();});

    const vector<Match> matches({
        // Handle requests to <http://localhost/> internally.
        Match( reqexpr::from_regex(host_getter, "localhost")
             , {false, queue<fresh_channel>({fresh_channel::_front_end})} ),

        Match( reqexpr::from_regex(x_oui_dest_getter, "OuiClient")
             , {false, queue<fresh_channel>({fresh_channel::_front_end})} ),

        // Access to sites under the `.local` TLD are always accessible
        // with good connectivity, so always use the Origin channel
        // and never cache them.
        Match( reqexpr::from_regex(target_getter, "https?://[^:/]+\\.local(:[0-9]+)?/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        // NOTE: The matching of HTTP methods below can be simplified,
        // leaving expanded for readability.

        // Send unsafe HTTP method requests to the origin server
        // (or the proxy if that does not work).
        // NOTE: The cache need not be disabled as it should know not to
        // fetch requests in these cases.
        Match( !reqexpr::from_regex(method_getter, "(GET|HEAD|OPTIONS|TRACE)")
             , nocache_request_config),
        // Do not use cache for safe but uncacheable HTTP method requests.
        // NOTE: same as above.
        Match( reqexpr::from_regex(method_getter, "(OPTIONS|TRACE)")
             , nocache_request_config),
        // Do not use cache for validation HEADs.
        // Caching these is not yet supported.
        Match( reqexpr::from_regex(method_getter, "HEAD")
             , nocache_request_config),

        // Disable cache and always go to origin for this site.
        Match( reqexpr::from_regex(target_getter, "https?://ident.me/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        // Disable cache and always go to proxy for this site.
        Match( reqexpr::from_regex(target_getter, "https?://ifconfig.co/.*")
             , {false, queue<fresh_channel>({fresh_channel::proxy})} ),
        // Force cache and default channels for this site.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.com/.*")
             , {true, queue<fresh_channel>()} ),
        // Force cache and particular channels for this site.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.net/.*")
             , {true, queue<fresh_channel>({fresh_channel::injector})} ),
    });

    auto connection_id = _next_connection_id++;

    // Is MitM active?
    bool mitm = false;

    // Saved host/port from CONNECT request.
    string connect_hp;
    // Process the different requests that may come over the same connection.
    for (;;) {  // continue for next request; break for no more requests
        // Read the (clear-text) HTTP request
        // (without a size limit, in case we are uploading a big file).
        // Based on <https://stackoverflow.com/a/50359998>.
        http::request_parser<Request::body_type> reqhp;
        reqhp.body_limit((std::numeric_limits<std::uint64_t>::max)());
        http::async_read(con, buffer, reqhp, yield_[ec]);

        Yield yield(con.get_io_service(), yield_, util::str('C', connection_id));

        if ( ec == http::error::end_of_stream
          || ec == asio::ssl::error::stream_truncated) break;

        if (ec) {
            cerr << "Failed to read request: " << ec.message() << endl;
            return;
        }

        Request req(reqhp.release());
        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done"); });
        auto target = req.target();

        // Perform MitM for CONNECT requests (to be able to see encrypted requests)
        if (!mitm && req.method() == http::verb::connect) {
            sys::error_code ec;
            // Subsequent access to the connection will use the encrypted channel.
            con = ssl_mitm_handshake(move(con), req, yield[ec].tag("mitm_hanshake"));
            if (ec) {
                yield.log("Mitm exception: ", ec.message());
                return;
            }
            mitm = true;
            // Save CONNECT target (minus standard HTTPS port ``:443`` if present)
            // in case of subsequent HTTP/1.0 requests with no ``Host:`` header.
            auto port_pos = max( target.length() - 4 /* strlen(":443") */
                               , string::npos);
            connect_hp = target
                // Do not to hit ``:443`` inside of an IPv6 address.
                .substr(0, target.rfind(":443", port_pos))
                .to_string();
            // Go for requests in the encrypted channel.
            continue;
        }

        if (maybe_handle_websocket_upgrade( con
                                          , connect_hp
                                          , req
                                          , yield[ec].tag("websocket"))) {
            break;
        }

        // Ensure that the request is proxy-like.
        if (!(target.starts_with("https://") || target.starts_with("http://"))) {
            if (mitm) {
                // Requests in the encrypted channel are usually not proxy-like
                // so the target is not "http://example.com/foo" but just "/foo".
                // We expand the target again with the ``Host:`` header
                // (or the CONNECT target if the header is missing in HTTP/1.0)
                // so that "/foo" becomes "https://example.com/foo".
                req.target( string("https://")
                          + ( (req[http::field::host].length() > 0)
                              ? req[http::field::host].to_string()
                              : connect_hp)
                          + target.to_string());
                target = req.target();
            } else {
                // TODO: Maybe later we want to support front-end and API calls
                // as plain HTTP requests (as if we were a plain HTTP server)
                // but for the moment we only accept proxy requests.
                handle_bad_request( con
                                  , req
                                  , "Not a proxy request"
                                  , yield.tag("handle_bad_request"));
                if (req.keep_alive()) continue;
                else return;
            }
        }

        request_config = route_choose_config(req, matches, default_request_config);

        auto res = cache_control.fetch(req, yield[ec].tag("cache_control.fetch"));

        if (ec) {
#ifndef NDEBUG
            yield.log("----- WARNING: Error fetching --------");
            yield.log("Error Code: ", ec.message());
            yield.log(req.base(), res.base());
            yield.log("--------------------------------------");
#endif

            // TODO: Better error message.
            handle_bad_request( con
                              , req
                              , "Not cached"
                              , yield.tag("handle_bad_request"));

            if (req.keep_alive()) continue;
            else return;
        }

        if (handle_if_injector_error(con, res, yield[ec])) {
            if (res.keep_alive()) continue;
            break;
        }

        yield.log("=== Sending back response ===");
        yield.log(res.base());

        // Forward the response back
        http::async_write(con, res, yield[ec].tag("write_response"));

        if (ec == http::error::end_of_stream) {
          LOG_DEBUG("request served. Connection closed");
          break;
        }

        if (ec) {
            yield.log("error writing back response: ", ec.message());
            return;
        }

        if (!res.keep_alive()) {
            con.close();
            break;
        }

        LOG_DEBUG("request served");
    }
}

//------------------------------------------------------------------------------
void Client::State::setup_ipfs_cache()
{
    if (_is_ipns_being_setup) {
        return;
    }

    _is_ipns_being_setup = true;

    asio::spawn(_ios, [ this
                      , self = shared_from_this()
                      ] (asio::yield_context yield) {
        if (was_stopped()) return;

        const string ipns = _config.ipns();

        if (_config.cache_enabled())
        {
            LOG_DEBUG("Starting IPFS Cache with IPNS ID: ", ipns);
            LOG_DEBUG("And BitTorrent pubkey: ", _config.bt_pub_key());

            auto on_exit = defer([&] { _is_ipns_being_setup = false; });

            if (ipns.empty()) {
                LOG_WARN("Support for IPFS Cache is disabled because we have not been provided with an IPNS id");
                _cache = nullptr;
                return;
            }

            if (_cache) {
                return _cache->set_ipns(move(ipns));
            }

            function<void()> cancel;

            auto cancel_slot = _shutdown_signal.connect([&] {
                if (cancel) cancel();
            });

            sys::error_code ec;
            _cache = CacheClient::build(_ios
                                       , ipns
                                       , _config.bt_pub_key()
                                       , _config.repo_root()
                                       , cancel
                                       , yield[ec]);

            if (ec) {
                cerr << "Failed to build CacheClient: "
                     << ec.message()
                     << endl;
            }
        }

        if (ipns != _config.ipns()) {
            // Use requested yet another IPNS
            setup_ipfs_cache();
        }
    });
}

//------------------------------------------------------------------------------
void Client::State::listen_tcp
        ( asio::yield_context yield
        , tcp::endpoint local_endpoint
        , function<void(GenericStream, asio::yield_context)> handler)
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(_ios);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) {
        cerr << "Failed to open tcp acceptor: " << ec.message() << endl;
        return;
    }

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) {
        cerr << "Failed to bind tcp acceptor: " << ec.message() << endl;
        return;
    }

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) {
        cerr << "Failed to 'listen' on tcp acceptor: " << ec.message() << endl;
        return;
    }

    auto shutdown_acceptor_slot = _shutdown_signal.connect([&acceptor] {
        acceptor.close();
    });

    LOG_DEBUG("Successfully listening on TCP Port");
    cout << "Client accepting on " << acceptor.local_endpoint() << endl;

    WaitCondition wait_condition(_ios);

    for(;;)
    {
        tcp::socket socket(_ios);
        acceptor.async_accept(socket, yield[ec]);

        if(ec) {
            if (ec == asio::error::operation_aborted) break;

            cerr << "Accept failed on tcp acceptor: " << ec.message() << endl;

            if (!async_sleep(_ios, chrono::seconds(1), _shutdown_signal, yield)) {
                break;
            }
        } else {
            static const auto tcp_shutter = [](tcp::socket& s) {
                sys::error_code ec; // Don't throw
                s.shutdown(tcp::socket::shutdown_both, ec);
                s.close(ec);
            };

            GenericStream connection(move(socket) , move(tcp_shutter));

            asio::spawn( _ios
                       , [ this
                         , self = shared_from_this()
                         , c = move(connection)
                         , handler
                         , lock = wait_condition.lock()
                         ](asio::yield_context yield) mutable {
                             if (was_stopped()) return;
                             handler(move(c), yield);
                         });
        }
    }

    wait_condition.wait(yield);
}

//------------------------------------------------------------------------------
void load_tls_ca_certificates(asio::ssl::context& ctx, const string& path_str)
{
    if (path_str.empty()) return;

    fs::path path = path_str;

    if (!exists(path)) {
        throw runtime_error(util::str(
                    "Can not read CA certificates from \"", path, "\": "
                    "No such file or directory"));
    }

    if (fs::is_directory(path)) {
        ctx.add_verify_path(path_str);
        return;
    }

    stringstream ss;
    ss << fs::ifstream(path).rdbuf();
    ctx.add_certificate_authority(asio::buffer(ss.str()));
}

//------------------------------------------------------------------------------
void Client::State::start()
{
    load_tls_ca_certificates(ssl_ctx, _config.tls_ca_cert_store_path());

    _ca_certificate = get_or_gen_tls_cert<CACertificate>
        ( "Your own local Ouinet client"
        , ca_cert_path(), ca_key_path(), ca_dh_path());

    if (!_config.tls_injector_cert_path().empty()) {
        if (fs::exists(fs::path(_config.tls_injector_cert_path()))) {
            LOG_DEBUG("Loading injector certificate file");
            inj_ctx.load_verify_file(_config.tls_injector_cert_path());
            LOG_DEBUG("Success");
        } else {
            throw runtime_error(
                    util::str("Invalid path to Injector's TLS cert file: "
                             , _config.tls_injector_cert_path()));
        }
    }

    asio::spawn
        ( _ios
        , [this, self = shared_from_this()]
          (asio::yield_context yield) {
              if (was_stopped()) return;

              sys::error_code ec;

              setup_injector(yield[ec]);

              if (was_stopped()) return;

              if (ec) {
                  cerr << "Failed to setup injector: "
                       << ec.message()
                       << endl;
              }

              setup_ipfs_cache();

              listen_tcp( yield[ec]
                        , _config.local_endpoint()
                        , [this, self]
                          (GenericStream c, asio::yield_context yield) {
                      serve_request(move(c), yield);
                  });
          });

    if (_config.front_end_endpoint() != tcp::endpoint()) {
        asio::spawn
            ( _ios
            , [this, self = shared_from_this()]
              (asio::yield_context yield) {
                  if (was_stopped()) return;

                  sys::error_code ec;

                  auto ep = _config.front_end_endpoint();
                  if (ep == tcp::endpoint()) return;

                  listen_tcp( yield[ec]
                            , ep
                            , [this, self]
                              (GenericStream c, asio::yield_context yield) {
                        sys::error_code ec;
                        Request rq;
                        beast::flat_buffer buffer;
                        http::async_read(c, buffer, rq, yield[ec]);

                        if (ec) return;

                        auto rs = _front_end.serve( _config
                                                  , rq
                                                  , _cache.get()
                                                  , *_ca_certificate
                                                  , yield[ec]);
                        if (ec) return;

                        http::async_write(c, rs, yield[ec]);
                  });
              });
    }
}

//------------------------------------------------------------------------------
void Client::State::setup_injector(asio::yield_context yield)
{
    _injector = std::make_unique<OuiServiceClient>(_ios);

    auto injector_ep = _config.injector_endpoint();

    if (!injector_ep) return;

    cout << "Setting up injector: " << *injector_ep << endl;

    std::unique_ptr<OuiServiceImplementationClient> client;

    if (injector_ep->type == Endpoint::I2pEndpoint) {
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((_config.repo_root()/"i2p").string(), _ios);
        auto i2p_client = i2p_service->build_client(injector_ep->endpoint_string);

        /*
        if (!i2p_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        */
        client = std::move(i2p_client);
    } else if (injector_ep->type == Endpoint::TcpEndpoint) {
        auto tcp_client = make_unique<ouiservice::TcpOuiServiceClient>(_ios, injector_ep->endpoint_string);

        if (!tcp_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(tcp_client);
    } else if (injector_ep->type == Endpoint::Obfs2Endpoint) {
        auto obfs2_client = make_unique<ouiservice::Obfs2OuiServiceClient>(_ios, injector_ep->endpoint_string, _config.repo_root()/"obfs2-client");

        if (!obfs2_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(obfs2_client);
    } else if (injector_ep->type == Endpoint::Obfs3Endpoint) {
        auto obfs3_client = make_unique<ouiservice::Obfs3OuiServiceClient>(_ios, injector_ep->endpoint_string, _config.repo_root()/"obfs3-client");

        if (!obfs3_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(obfs3_client);
    } else if (injector_ep->type == Endpoint::Obfs4Endpoint) {
        auto obfs4_client = make_unique<ouiservice::Obfs4OuiServiceClient>(_ios, injector_ep->endpoint_string, _config.repo_root()/"obfs4-client");

        if (!obfs4_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(obfs4_client);
    }

    bool enable_injector_tls = !_config.tls_injector_cert_path().empty();
    if (!enable_injector_tls) {
        _injector->add(std::move(client));
    } else {
        auto tls_client
            = make_unique<ouiservice::TlsOuiServiceClient>(move(client), inj_ctx);
        _injector->add(std::move(tls_client));
    }

    _injector->start(yield);
}

//------------------------------------------------------------------------------
void Client::State::set_injector(string injector_ep_str)
{
    // XXX: Workaround.
    // Eventually, OuiServiceClient should just support multiple parallel
    // active injector EPs.

    auto injector_ep = parse_endpoint(injector_ep_str);

    if (!injector_ep) {
        cerr << "Failed to parse endpoint \"" << injector_ep_str << "\"" << endl;
        return;
    }

    auto current_ep = _config.injector_endpoint();

    if (current_ep && *injector_ep == *current_ep) {
        return;
    }

    _config.set_injector_endpoint(*injector_ep);

    asio::spawn(_ios, [self = shared_from_this(), injector_ep_str] (auto yield) {
            if (self->was_stopped()) return;
            sys::error_code ec;
            self->setup_injector(yield[ec]);

            if (ec == asio::error::invalid_argument) {
                cerr << "Failed to parse endpoint \"" << injector_ep_str << "\"" << endl;
            }
        });
}

//------------------------------------------------------------------------------
Client::Client(asio::io_service& ios, ClientConfig cfg)
    : _state(make_shared<State>(ios, move(cfg)))
{}

Client::~Client()
{
}

void Client::start()
{
    _state->start();
}

void Client::stop()
{
    _state->stop();
}

void Client::set_injector_endpoint(const char* injector_ep)
{
    _state->set_injector(injector_ep);
}

void Client::set_ipns(const char* ipns)
{
    _state->_config.set_ipns(move(ipns));
    _state->setup_ipfs_cache();
}

void Client::set_credentials(const char* injector, const char* cred)
{
    _state->_config.set_credentials(injector, cred);
}

fs::path Client::ca_cert_path() const
{
    return _state->ca_cert_path();
}

//------------------------------------------------------------------------------
#ifndef __ANDROID__
int main(int argc, char* argv[])
{
    ClientConfig cfg;

    try {
        cfg = ClientConfig(argc, argv);
    } catch(std::exception const& e) {
        LOG_ABORT(e.what());
        return 1;
    }

    if (cfg.is_help()) {
        cout << "Usage:" << endl;
        cout << cfg.description() << endl;
        return 0;
    }

    util::crypto_init();

    asio::io_service ios;

    asio::signal_set signals(ios, SIGINT, SIGTERM);

    Client client(ios, move(cfg));

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&client, &signals, &ios, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            client.stop();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    try {
        client.start();
    } catch (std::exception& e) {
        cerr << e.what() << endl;
        return 1;
    }

    ios.run();

    return EXIT_SUCCESS;
}
#endif
