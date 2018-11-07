#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/optional/optional_io.hpp>
#include <iostream>
#include <fstream>
#include <cstdlib>  // for atexit()

#include "cache/cache_client.h"

#include "namespaces.h"
#include "origin_pools.h"
#include "http_util.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_stream.h"
#include "util.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
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
#include "ssl/ca_certificate.h"
#include "ssl/dummy_certificate.h"
#include "ssl/util.h"

#ifndef __ANDROID__
#  include "force_exit_on_signal.h"
#endif // ifndef __ANDROID__

#include "ouiservice.h"
#include "ouiservice/i2p.h"
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
static const fs::path OUINET_INJ_CERT_FILE = "ssl-inj-cert.pem";

//------------------------------------------------------------------------------
class Client::State : public enable_shared_from_this<Client::State> {
    friend class Client;

public:
    State(asio::io_service& ios)
        : _ios(ios)
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

    void start(int argc, char* argv[]);

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

    void handle_connect_request( GenericStream& client_c
                               , const Request& req
                               , Yield yield);

    CacheEntry
    fetch_stored( const Request& request
                , request_route::Config& request_config
                , Cancel& cancel
                , asio::yield_context yield);

    Response fetch_fresh( const Request&
                        , request_route::Config&
                        , bool& out_can_store
                        , Cancel& cancel
                        , Yield);

    Response fetch_fresh_from_origin(const Request&, Cancel&, Yield);

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

private:
    asio::io_service& _ios;
    std::unique_ptr<CACertificate> _ca_certificate;
    util::LruCache<string, string> _ssl_certificate_cache;
    ClientConfig _config;
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
void Client::State::handle_connect_request( GenericStream& client_c
                                          , const Request& req
                                          , Yield yield)
{
    // https://tools.ietf.org/html/rfc2817#section-5.2

    sys::error_code ec;

    if (!_front_end.is_injector_proxying_enabled()) {
        return handle_bad_request( client_c
                                 , req
                                 , "Forwarding disabled"
                                 , yield[ec]);
    }

    auto inj = _injector->connect(yield[ec], _shutdown_signal);

    if (ec) {
        // TODO: Does an RFC dicate a particular HTTP status code?
        return handle_bad_request(client_c, req, "Can't connect to injector", yield[ec]);
    }

    auto disconnect_injector_slot = _shutdown_signal.connect([&inj] {
        inj.connection.close();
    });

    auto credentials = _config.credentials_for(inj.remote_endpoint);

    if (credentials) {
        auto auth_req = authorize(req, *credentials);
        http::async_write(inj.connection, auth_req, yield[ec]);
    }
    else {
        http::async_write(inj.connection, const_cast<Request&>(req), yield[ec]);
    }

    if (ec) {
        // TODO: Does an RFC dicate a particular HTTP status code?
        return handle_bad_request(client_c, req, "Can't contact the injector", yield[ec]);
    }

    beast::flat_buffer buffer;
    Response res;
    http::async_read(inj.connection, buffer, res, yield[ec]);

    if (ec) {
        // TODO: Does an RFC dicate a particular HTTP status code?
        return handle_bad_request(client_c, req, "Can't contact the injector", yield[ec]);
    }

    http::async_write(client_c, res, yield[ec]);

    if (ec) {
        cerr << "Failed to return CONNECT response: " << ec.message() << endl;
        return;
    }

    if (!(200 <= unsigned(res.result()) && unsigned(res.result()) < 300)) {
        return;
    }

    full_duplex(move(client_c), move(inj.connection), yield);
}

//------------------------------------------------------------------------------
CacheEntry
Client::State::fetch_stored( const Request& request
                           , request_route::Config& request_config
                           , Cancel& cancel
                           , asio::yield_context yield)
{
    const bool cache_is_disabled
        = !request_config.enable_cache
       || !_cache
       || !_front_end.is_ipfs_cache_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheEntry>( yield
                                   , asio::error::operation_not_supported);
    }

    // TODO: use string_view for the key.
    auto key = request.target();

    sys::error_code ec;
    auto ret = _cache->get_content( key.to_string()
                                  , _config.default_db_type()
                                  , cancel
                                  , yield[ec]);
    // Add an injection identifier header
    // to enable the user to track injection state.
    if (!ec)
        ret.second.response.set(http_::response_injection_id_hdr, ret.first);
    return or_throw(yield, ec, move(ret.second));
}

//------------------------------------------------------------------------------
Response Client::State::fetch_fresh_from_origin( const Request& rq
                                               , Cancel& cancel_
                                               , Yield yield)
{
    Cancel cancel;
    auto cancel_slot = cancel_.connect([&] { cancel(); });

    WatchDog watch_dog(_ios
                      , default_timeout::fetch_http()
                      , [&] { cancel(); });

    auto con = _origin_pools.get_connection(rq);

    if (!con) {
        std::string host, port;
        std::tie(host, port) = util::get_host_port(rq);

        sys::error_code ec;

        auto lookup = util::tcp_async_resolve( host, port
                                             , _ios
                                             , cancel
                                             , yield[ec]);

        if (ec) return or_throw<Response>(yield, ec);

        auto sock = connect_to_host(lookup, _ios, cancel, yield[ec]);

        if (ec) return or_throw<Response>(yield, ec);

        GenericStream stream;

        if (rq.target().starts_with("https:")) {
            stream = ssl::util::client_handshake( move(sock)
                                                , ssl_ctx
                                                , host
                                                , cancel
                                                , yield[ec]);

            if (ec) return or_throw<Response>(yield, ec);
        }
        else {
            stream = move(sock);
        }

        con.reset(new ConnectionPool<>::Connection(move(stream), boost::none));
    }

    sys::error_code ec;

    // Transform request from absolute-form to origin-form
    // https://tools.ietf.org/html/rfc7230#section-5.3
    auto rq_ = req_form_from_absolute_to_origin(rq);

    auto res = con->request(rq_, cancel, yield[ec]);

    if (!ec && res.keep_alive()) {
        _origin_pools.insert_connection(rq, move(con));
    }

    return or_throw(yield, ec, move(res));
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
    using request_route::responder;

    auto shutdown_slot = _shutdown_signal.connect([&] { cancel(); });

    out_can_store = false;

    sys::error_code last_error = operation_not_supported;

    LOG_DEBUG("fetching fresh");

    while (!request_config.responders.empty()) {
        auto r = request_config.responders.front();
        request_config.responders.pop();

        switch (r) {
            case responder::origin: {
                if (!_front_end.is_origin_access_enabled()) {
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

                return res;
            }
            // Since the current implementation uses the injector as a proxy,
            // both cases are quite similar, so we only handle HTTPS requests here.
            case responder::proxy: {
                if (!_front_end.is_proxy_access_enabled())
                    continue;

                auto target = request.target();
                if (target.starts_with("https://")) {
                    // Parse the URL to tell HTTP/HTTPS, host, port.
                    util::url_match url;
                    if (!match_http_url(target, url)) {
                        last_error = asio::error::operation_not_supported;  // unsupported URL
                        continue;
                    }

                    // Connect to the injector/proxy.
                    sys::error_code ec;
                    auto inj = _injector->connect( yield[ec].tag("connect_to_injector")
                                                 , cancel);
                    if (ec) {
                        last_error = ec;
                        continue;
                    }

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
                        last_error = asio::error::connection_refused;
                        yield.tag("proxy_connect").log(connres);
                        continue;
                    }

                    // Send the request to the origin.
                    auto res = fetch_http_origin( _ios , inj.connection, ssl_ctx
                                                , url, request
                                                , default_timeout::fetch_http()
                                                , cancel
                                                , yield[ec].tag("send_req"));

                    if (ec) {
                        last_error = ec;
                        continue;
                    }

                    return res;
                }
            }
            // Fall through, the case below handles both injector and proxy with plain HTTP.
            case responder::injector: {
                if (r == responder::injector && !_front_end.is_injector_proxying_enabled())
                    continue;

                // Connect to the injector.
                sys::error_code ec;

                using Con = ConnectionPool<std::string>::Connection;

                unique_ptr<Con> con = _injector_connections.pop_front();

                if (!con) {
                    auto c = _injector->connect
                        (yield[ec].tag("connect_to_injector2"), cancel);

                    if (ec) { last_error = ec; continue; }

                    con = make_unique<Con>( move(c.connection)
                                          , move(c.remote_endpoint) );
                }

                // Build the actual request to send to the injector.
                Request injreq = request;

                if (r == responder::injector) {
                    // Add first a Ouinet version header
                    // to hint it to behave like an injector instead of a proxy.
                    injreq.set( http_::request_version_hdr
                              , http_::request_version_hdr_latest);
                }

                if (auto credentials = _config.credentials_for(con->aux))
                    injreq = authorize(injreq, *credentials);

                // Send the request to the injector/proxy.
                auto res = con->request( injreq
                                       , cancel
                                       , yield[ec].tag("inj-request"));

                if (ec) {
                    last_error = ec;
                    continue;
                }

                out_can_store = (r == responder::injector);

                if (res.keep_alive()) {
                    _injector_connections.push_back(std::move(con));
                }

                return res;
            }
            case responder::_front_end: {
                sys::error_code ec;

                auto res = _front_end.serve( _config.injector_endpoint()
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
        , cc(client_state.get_io_service(), "Ouinet Client")
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
        sys::error_code ec;

        auto& cache = client_state._cache;

        if (!_can_store) ec = asio::error::invalid_argument;
        if (!cache)      ec = asio::error::operation_not_supported;

        if (ec) return or_throw(yield, ec, move(rs));

        asio::spawn(client_state.get_io_service(),
            [&, rs] (asio::yield_context yield) {
                // TODO: Use the scheduler here to only do some max number
                // of `ipfs_add`s at a time. Also then trim that queue so
                // that it doesn't grow indefinitely.
                sys::error_code ec;
                cache->ipfs_add( beast::buffers_to_string(rs.body().data())
                               , yield[ec]);
            });

        // Note: we have to return a valid response even in case of error
        // because CacheControl will use it.
        return or_throw(yield, ec, move(rs));
    }

    Response fetch(const Request& rq, Yield yield)
    {
        return cc.fetch(rq, yield);
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
void Client::State::serve_request( GenericStream&& con
                                 , asio::yield_context yield_)
{
    LOG_DEBUG("Request received ");

    namespace rr = request_route;
    using rr::responder;

    auto close_con_slot = _shutdown_signal.connect([&con] {
        con.close();
    });

    // These access mechanisms are attempted in order for requests by default.
    const rr::Config default_request_config
        { true
        , queue<responder>({responder::injector})};

    rr::Config request_config;

    Client::ClientCacheControl cache_control(*this, request_config);

    sys::error_code ec;
    beast::flat_buffer buffer;

    // Expressions to test the request against and mechanisms to be used.
    // TODO: Create once and reuse.
    using Match = pair<const ouinet::reqexpr::reqex, const rr::Config>;

    auto method_getter([](const Request& r) {return r.method_string();});
    auto host_getter([](const Request& r) {return r["Host"];});
    auto x_oui_dest_getter([](const Request& r) {return r["X-Oui-Destination"];});
    auto target_getter([](const Request& r) {return r.target();});

    const vector<Match> matches({
        // Handle requests to <http://localhost/> internally.
        Match( reqexpr::from_regex(host_getter, "localhost")
             , {false, queue<responder>({responder::_front_end})} ),

        Match( reqexpr::from_regex(x_oui_dest_getter, "OuiClient")
             , {false, queue<responder>({responder::_front_end})} ),

        // NOTE: The matching of HTTP methods below can be simplified,
        // leaving expanded for readability.

        // Send unsafe HTTP method requests to the origin server
        // (or the proxy if that does not work).
        // NOTE: The cache need not be disabled as it should know not to
        // fetch requests in these cases.
        Match( !reqexpr::from_regex(method_getter, "(GET|HEAD|OPTIONS|TRACE)")
             , {false, queue<responder>({responder::origin, responder::proxy})} ),
        // Do not use cache for safe but uncacheable HTTP method requests.
        // NOTE: same as above.
        Match( reqexpr::from_regex(method_getter, "(OPTIONS|TRACE)")
             , {false, queue<responder>({responder::origin, responder::proxy})} ),
        // Do not use cache for validation HEADs.
        // Caching these is not yet supported.
        Match( reqexpr::from_regex(method_getter, "HEAD")
             , {false, queue<responder>({responder::origin, responder::proxy})} ),

        // Disable cache and always go to origin for this site.
        Match( reqexpr::from_regex(target_getter, "https?://ident.me/.*")
             , {false, queue<responder>({responder::origin})} ),
        // Disable cache and always go to proxy for this site.
        Match( reqexpr::from_regex(target_getter, "https?://ifconfig.co/.*")
             , {false, queue<responder>({responder::proxy})} ),
        // Force cache and default mechanisms for this site.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.com/.*")
             , {true, queue<responder>()} ),
        // Force cache and particular mechanisms for this site.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.net/.*")
             , {true, queue<responder>({responder::injector})} ),
    });

    auto connection_id = _next_connection_id++;

    // Is MitM active?
    bool mitm(false);
    // Saved host/port from CONNECT request.
    string connect_hp;
    // Process the different requests that may come over the same connection.
    for (;;) {  // continue for next request; break for no more requests
        Request req;

        // Read the (clear-text) HTTP request
        http::async_read(con, buffer, req, yield_[ec]);

        Yield yield(con.get_io_service(), yield_, util::str('C', connection_id));

        if ( ec == http::error::end_of_stream
          || ec == asio::ssl::error::stream_truncated) break;

        if (ec) {
            cerr << "Failed to read request: " << ec.message() << endl;
            return;
        }

        yield.log("=== New request ===");
        yield.log(req.base());
        auto on_exit = defer([&] { yield.log("Done"); });

        // Requests in the encrypted channel are not proxy-like
        // so the target is not "http://example.com/foo" but just "/foo".
        // We expand the target again with the ``Host:`` header
        // (or the CONNECT target if the header is missing in HTTP/1.0)
        // so that "/foo" becomes "https://example.com/foo".
        if (mitm) {
            req.target( string("https://")
                      + ( (req[http::field::host].length() > 0)
                          ? req[http::field::host].to_string()
                          : connect_hp)
                      + req.target().to_string());
        }

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
            auto port_pos = max( req.target().length() - 4 /* strlen(":443") */
                               , string::npos);
            connect_hp = req.target()
                // Do not to hit ``:443`` inside of an IPv6 address.
                .substr(0, req.target().rfind(":443", port_pos))
                .to_string();
            // Go for requests in the encrypted channel.
            continue;
        }

        // TODO: If an HTTPS request contains any private data (cookies, GET,
        // POST arguments...) it should be either routed to the Origin or a
        // Proxy (which may be the injector) using a CONNECT request (as is
        // done in the handle_connect_request function).

        //if (_config.enable_http_connect_requests()) {
        //    handle_connect_request(con, req, yield);
        //}
        //else {
        //    auto res = bad_gateway(req);
        //    http::async_write(con, res, yield[ec]);
        //}
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
void Client::State::start(int argc, char* argv[])
{
    try {
        _config = ClientConfig(argc, argv);
    } catch(std::exception const& e) {
        //explicit is better than implecit
        LOG_ABORT(e.what());
    }

    if (_config.is_help()) {
        cout << "Usage:" << endl;
        cout << _config.description() << endl;
        return;
    }

    _ca_certificate = get_or_gen_tls_cert<CACertificate>
        ( "Your own local Ouinet client"
        , ca_cert_path(), ca_key_path(), ca_dh_path());

    if (_config.enable_injector_tls()) {
        auto inj_cert_path = _config.repo_root() / OUINET_INJ_CERT_FILE;
        if (fs::exists(inj_cert_path)) {
            LOG_DEBUG("Loading injector certificate file");
            inj_ctx.load_verify_file(inj_cert_path.string());
        } else {
            LOG_DEBUG("Creating empty injector certificate file");
            fs::ofstream(inj_cert_path) << "";
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

                        auto rs = _front_end.serve( _config.injector_endpoint()
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

    if (is_i2p_endpoint(*injector_ep)) {
        std::string ep = boost::get<I2PEndpoint>(*injector_ep).pubkey;
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((_config.repo_root()/"i2p").string(), _ios);
        std::unique_ptr<ouiservice::I2pOuiServiceClient> i2p_client = i2p_service->build_client(ep);

        _injector->add(std::move(i2p_client));
    } else {
        tcp::endpoint tcp_endpoint
            = boost::get<asio::ip::tcp::endpoint>(*injector_ep);

        auto tcp_client
            = make_unique<ouiservice::TcpOuiServiceClient>(_ios, tcp_endpoint);

        if (!_config.enable_injector_tls()) {
            _injector->add(std::move(tcp_client));
        } else {
            auto tls_client
                = make_unique<ouiservice::TlsOuiServiceClient>(move(tcp_client), inj_ctx);
            _injector->add(std::move(tls_client));
        }
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

    asio::spawn(_ios, [self = shared_from_this()] (auto yield) {
            if (self->was_stopped()) return;
            sys::error_code ec;
            self->setup_injector(yield[ec]);
        });
}

//------------------------------------------------------------------------------
Client::Client(asio::io_service& ios)
    : _state(make_shared<State>(ios))
{}

Client::~Client()
{
}

void Client::start(int argc, char* argv[])
{
    _state->start(argc, argv);
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
    util::crypto_init();

    asio::io_service ios;

    asio::signal_set signals(ios, SIGINT, SIGTERM);

    Client client(ios);

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&client, &signals, &ios, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            client.stop();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    try {
        client.start(argc, argv);
    } catch (std::exception& e) {
        cerr << e.what() << endl;
        return 1;
    }

    ios.run();

    return EXIT_SUCCESS;
}
#endif
