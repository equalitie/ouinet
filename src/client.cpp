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
#include <lrucache.hpp>
#include <iostream>
#include <fstream>
#include <cstdlib>  // for atexit()

#include "cache/cache_client.h"
#include "cache/http_desc.h"

#include "namespaces.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_connection.h"
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
#include "ssl/ca_certificate.h"
#include "ssl/dummy_certificate.h"

#ifndef __ANDROID__
#  include "force_exit_on_signal.h"
#endif // ifndef __ANDROID__

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"

#include "util/signal.h"

#include "logger.h"

using namespace std;
using namespace ouinet;

namespace posix_time = boost::posix_time;

using tcp      = asio::ip::tcp;
using Request  = http::request<http::string_body>;
using Response = http::response<http::dynamic_body>;
using boost::optional;

static const boost::filesystem::path OUINET_PID_FILE = "pid";
static const boost::filesystem::path OUINET_CA_CERT_FILE = "ssl-ca-cert.pem";
static const boost::filesystem::path OUINET_CA_KEY_FILE = "ssl-ca-key.pem";
static const boost::filesystem::path OUINET_CA_DH_FILE = "ssl-ca-dh.pem";

//------------------------------------------------------------------------------
#define ASYNC_DEBUG(code, ...) [&] () mutable {\
    auto task = _front_end.notify_task(util::str(__VA_ARGS__));\
    return code;\
}()

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
    { }

    void start(int argc, char* argv[]);

    void stop() {
        _ipfs_cache = nullptr;
        _shutdown_signal();
        if (_injector) _injector->stop();
    }

    void setup_ipfs_cache();
    void set_injector(string);

private:
    GenericConnection ssl_mitm_handshake( GenericConnection&&
                                        , const Request&
                                        , asio::yield_context);

    void serve_request(GenericConnection&& con, asio::yield_context yield);

    void handle_connect_request( GenericConnection& client_c
                               , const Request& req
                               , asio::yield_context yield);

    CacheControl::CacheEntry
    fetch_stored( const Request& request
                , request_route::Config& request_config
                , asio::yield_context yield);

    Response fetch_fresh( const Request& request
                        , request_route::Config& request_config
                        , asio::yield_context yield);

    CacheControl build_cache_control(request_route::Config& request_config);

    void listen_tcp( asio::yield_context
                   , tcp::endpoint
                   , function<void(GenericConnection, asio::yield_context)>);

    void setup_injector(asio::yield_context);

    boost::filesystem::path get_pid_path() const {
        return _config.repo_root()/OUINET_PID_FILE;
    }

    string maybe_start_seeding( const Request&
                              , const Response&
                              , asio::yield_context);

    bool was_stopped() const {
        return _shutdown_signal.call_count() != 0;
    }

private:
    asio::io_service& _ios;
    std::unique_ptr<CACertificate> _ca_certificate;
    cache::lru_cache<string, string> _ssl_certificate_cache;
    ClientConfig _config;
    std::unique_ptr<OuiServiceClient> _injector;
    std::unique_ptr<CacheClient> _ipfs_cache;

    ClientFrontEnd _front_end;
    Signal<void()> _shutdown_signal;

    unique_ptr<util::PidFile> _pid_file;

    bool _is_ipns_being_setup = false;
};

//------------------------------------------------------------------------------
string Client::State::maybe_start_seeding( const Request&  req
                                         , const Response& res
                                         , asio::yield_context yield)
{
    if (!_ipfs_cache)
        return or_throw<string>(yield, asio::error::operation_not_supported);

    const char* reason = "";
    if (!CacheControl::ok_to_cache(req, res, &reason)) {
        cerr << "---------------------------------------" << endl;
        cerr << "Not caching " << req.target() << endl;
        cerr << "Because: \"" << reason << "\"" << endl;
        cerr << req.base() << res.base();
        cerr << "---------------------------------------" << endl;
        return {};
    }

    return _ipfs_cache->ipfs_add
            ( util::str(CacheControl::filter_before_store(res))
            , yield);
}

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
void Client::State::handle_connect_request( GenericConnection& client_c
                                          , const Request& req
                                          , asio::yield_context yield)
{
    // https://tools.ietf.org/html/rfc2817#section-5.2

    sys::error_code ec;

    if (!_front_end.is_injector_proxying_enabled()) {
        return ASYNC_DEBUG( handle_bad_request( client_c
                                              , req
                                              , "Forwarding disabled"
                                              , yield[ec])
                          , "Forwarding disabled");
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

    if (ec) return fail(ec, "sending connect response");

    if (!(200 <= unsigned(res.result()) && unsigned(res.result()) < 300)) {
        return;
    }

    full_duplex(client_c, inj.connection, yield);
}

//------------------------------------------------------------------------------
CacheControl::CacheEntry
Client::State::fetch_stored( const Request& request
                           , request_route::Config& request_config
                           , asio::yield_context yield)
{
    using CacheEntry = CacheControl::CacheEntry;

    const bool cache_is_disabled
        = !request_config.enable_cache
       || !_ipfs_cache
       || !_front_end.is_ipfs_cache_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheControl::CacheEntry>( yield ,
                asio::error::operation_not_supported);
    }

    sys::error_code ec;
    // Get the content from cache
    auto key = request.target();

    auto content = _ipfs_cache->get_content(key.to_string(), yield[ec]);

    if (ec) return or_throw<CacheEntry>(yield, ec);

    // If the content does not have a meaningful time stamp,
    // an error should have been reported.
    assert(!content.ts.is_not_a_date_time());

    auto res = descriptor::http_parse(*_ipfs_cache, content.data, yield[ec]);
    return or_throw(yield, ec, CacheEntry{content.ts, res});
}

//------------------------------------------------------------------------------
Response Client::State::fetch_fresh( const Request& request
                                   , request_route::Config& request_config
                                   , asio::yield_context yield)
{
    using namespace asio::error;
    using request_route::responder;

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
                Response res;

                // Send the request straight to the origin
                res = fetch_http_page(_ios, request, _shutdown_signal, yield[ec]);

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
                    if (!match_http_url(target.to_string(), url)) {
                        last_error = asio::error::operation_not_supported;  // unsupported URL
                        continue;
                    }

                    // Connect to the injector/proxy.
                    sys::error_code ec;
                    auto inj = _injector->connect(yield[ec], _shutdown_signal);
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
                    auto connres = fetch_http_head( _ios
                                                  , inj.connection
                                                  , connreq
                                                  , _shutdown_signal
                                                  , yield[ec]);
                    if (connres.result() != http::status::ok) {
                        // This error code is quite fake, so log the error too.
                        last_error = asio::error::connection_refused;
                        cerr << "Failed HTTP CONNECT to " << connreq.target() << ": "
                             << connres.result_int() << " " << connres.reason() << endl;
                        continue;
                    }

                    // Send the request to the origin.
                    auto res = fetch_http_origin( _ios , inj.connection
                                                , url, request
                                                , _shutdown_signal
                                                , yield[ec]);
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
                auto inj = _injector->connect(yield[ec], _shutdown_signal);
                if (ec) {
                    last_error = ec;
                    continue;
                }

                // Build the actual request to send to the injector.
                Request injreq(request);
                if (r == responder::injector)
                    // Add first a Ouinet version header
                    // to hint it to behave like an injector instead of a proxy.
                    injreq.set(request_version_hdr, request_version_hdr_latest);
                if (auto credentials = _config.credentials_for(inj.remote_endpoint))
                    injreq = authorize(injreq, *credentials);

                // Send the request to the injector/proxy.
                auto res = fetch_http_page( _ios
                                          , inj.connection
                                          , injreq
                                          , _shutdown_signal
                                          , yield[ec]);
                if (ec) {
                    last_error = ec;
                    continue;
                }

                if (r == responder::injector) {
                    sys::error_code ec_;  // seed the original request
                    string ipfs = maybe_start_seeding(request, res, yield[ec_]);
                }

                return res;
            }
            case responder::_front_end: {
                sys::error_code ec;
                auto res = _front_end.serve( _config.injector_endpoint()
                                           , request
                                           , _ipfs_cache.get()
                                           , *_ca_certificate
                                           , yield[ec]);
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
CacheControl
Client::State::build_cache_control(request_route::Config& request_config)
{
    CacheControl cache_control;

    cache_control.fetch_stored =
        [&] (const Request& request, asio::yield_context yield) {

            cerr << "Fetching from cache " << request.target() << endl;

            sys::error_code ec;
            auto r = ASYNC_DEBUG( fetch_stored(request, request_config, yield[ec])
                              , "Fetch from cache: " , request.target());

            cerr << "Fetched from cache " << request.target()
                 << " " << ec.message() << " " << r.response.result()
                 << endl;

            return or_throw(yield, ec, move(r));
        };

    cache_control.fetch_fresh =
        [&] (const Request& request, asio::yield_context yield) {

            cerr << "Fetching fresh " << request.target() << endl;

            sys::error_code ec;
            auto r = ASYNC_DEBUG( fetch_fresh(request, request_config, yield[ec])
                              , "Fetch from origin: ", request.target());

            cerr << "Fetched fresh " << request.target()
                 << " " << ec.message() << " " << r.result()
                 << endl;

            return or_throw(yield, ec, move(r));
        };

    cache_control.max_cached_age(_config.max_cached_age());

    return cache_control;
}

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
void setup_ssl_context( asio::ssl::context& ssl_context
                      , const string& cert_chain
                      , const string& private_key
                      , const string& dh)
{
    namespace ssl = boost::asio::ssl;

    ssl_context.set_options( ssl::context::default_workarounds
                           | ssl::context::no_sslv2
                           | ssl::context::single_dh_use);

    ssl_context.use_certificate_chain(
            asio::buffer(cert_chain.data(), cert_chain.size()));

    ssl_context.use_private_key( asio::buffer( private_key.data()
                                             , private_key.size())
                               , ssl::context::file_format::pem);

    ssl_context.use_tmp_dh(asio::buffer(dh.data(), dh.size()));

    ssl_context.set_password_callback(
        [](std::size_t, ssl::context_base::password_purpose)
        {
            assert(0 && "TODO: Not yet supported");
            return "";
        });
}

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
GenericConnection Client::State::ssl_mitm_handshake( GenericConnection&& con
                                                   , const Request& con_req
                                                   , asio::yield_context yield)
{
    namespace ssl = boost::asio::ssl;

    ssl::context ssl_context{ssl::context::tls_server};

    // TODO: We really should be waiting for
    // the TLS Client Hello message to arrive at the clear text connection
    // (after we send back 200 OK),
    // then retrieve the value of the Server Name Indication (SNI) field
    // and rewind the Hello message,
    // but for the moment we will assume that the browser sends
    // a host name instead of an IP address or its reverse resolution.
    auto base_domain = base_domain_from_target(con_req.target());

    string crt_chain;
    // TODO: ASan gets confused when an exception is thrown inside a coroutine,
    // the alternative is to check ``_ssl_certificate_cache.exists(base_domain)``
    // (i.e. an additional lookup) then take one branch or the other.
    try {
        crt_chain = _ssl_certificate_cache.get(base_domain);
    } catch(const std::range_error&) {
        DummyCertificate dummy_crt(*_ca_certificate, base_domain);

        crt_chain = dummy_crt.pem_certificate()
                  + _ca_certificate->pem_certificate();

        _ssl_certificate_cache.put(move(base_domain), crt_chain);
    }

    setup_ssl_context( ssl_context
                     , crt_chain
                     , _ca_certificate->pem_private_key()
                     , _ca_certificate->pem_dh_param());

    // Send back OK to let the UA know we have the "tunnel"
    http::response<http::string_body> res{http::status::ok, con_req.version()};
    http::async_write(con, res, yield);

    sys::error_code ec;

    auto ssl_sock = make_unique<ssl::stream<GenericConnection>>(move(con), ssl_context);
    ssl_sock->async_handshake(ssl::stream_base::server, yield[ec]);
    if (ec) return or_throw<GenericConnection>(yield, ec);

    static const auto ssl_shutter = [](ssl::stream<GenericConnection>& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericConnection(move(ssl_sock), move(ssl_shutter));
}

//------------------------------------------------------------------------------
void Client::State::serve_request( GenericConnection&& con
                                 , asio::yield_context yield)
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

    CacheControl cache_control = build_cache_control(request_config);

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

        // NOTE: The injector mechanism is temporarily used in some matches
        // instead of the mechanisms following it (commented out)
        // since the later are not implemented yet.

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

    // Is MitM active?
    bool mitm(false);
    // Saved host/port from CONNECT request.
    string connect_hp;
    // Process the different requests that may come over the same connection.
    for (;;) {  // continue for next request; break for no more requests
        Request req;

        // Read the (clear-text) HTTP request
        ASYNC_DEBUG(http::async_read(con, buffer, req, yield[ec]), "Read request");

        if ( ec == http::error::end_of_stream
          || ec == asio::ssl::error::stream_truncated) break;

        if (ec) return fail(ec, "read");

        // Requests in the encrypted channel are not proxy-like
        // so the target is not "http://example.com/foo" but just "/foo".
        // We expand the target again with the ``Host:`` header
        // (or the CONNECT target if the header is missing in HTTP/1.0)
        // so that "/foo" becomes "https://example.com/foo".
        if (mitm)
            req.target( string("https://")
                      + ( (req[http::field::host].length() > 0)
                          ? req[http::field::host].to_string()
                          : connect_hp)
                      + req.target().to_string());
        cout << "Received request for: " << req.target() << endl;

        // Perform MitM for CONNECT requests (to be able to see encrypted requests)
        if (!mitm && req.method() == http::verb::connect) {
            try {
                // Subsequent access to the connection will use the encrypted channel.
                con = ssl_mitm_handshake(move(con), req, yield);
            }
            catch(const std::exception& e) {
                cerr << "Mitm exception: " << e.what() << endl;
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
        //    ASYNC_DEBUG( handle_connect_request(con, req, yield)
        //               , "Connect ", req.target());
        //}
        //else {
        //    auto res = bad_gateway(req);
        //    http::async_write(con, res, yield[ec]);
        //}
        request_config = route_choose_config(req, matches, default_request_config);

        auto res = ASYNC_DEBUG( cache_control.fetch(req, yield[ec])
                              , "Fetch "
                              , req.target());

        cout << "Sending back response: " << req.target() << " " << res.result() << endl;

        if (ec) {
#ifndef NDEBUG
            cerr << "----- WARNING: Error fetching --------" << endl;
            cerr << "Error Code: " << ec.message() << endl;
            cerr << req.base() << res.base() << endl;
            cerr << "--------------------------------------" << endl;
#endif

            // TODO: Better error message.
            ASYNC_DEBUG(handle_bad_request(con, req, "Not cached", yield), "Send error");
            if (req.keep_alive()) continue;
            else return;
        }

        cout << req.base() << res.base() << endl;
        // Forward the response back
        ASYNC_DEBUG(http::async_write(con, res, yield[ec]), "Write response ", req.target());
        if (ec == http::error::end_of_stream) {
          LOG_DEBUG("request served. Connection closed");
          break;
        }
        if (ec) return fail(ec, "write");
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

        {
            LOG_DEBUG("Starting IPFS Cache with IPNS ID: " + ipns);
            auto on_exit = defer([&] { _is_ipns_being_setup = false; });

            if (ipns.empty()) {
                LOG_WARN("Support for IPFS Cache is disabled because we have not been provided with an IPNS id");
                _ipfs_cache = nullptr;
                return;
            }

            if (_ipfs_cache) {
                return _ipfs_cache->set_ipns(move(ipns));
            }

            string repo_root = (_config.repo_root()/"ipfs").native();

            function<void()> cancel;

            auto cancel_slot = _shutdown_signal.connect([&] {
                if (cancel) cancel();
            });

            sys::error_code ec;
            _ipfs_cache = CacheClient::build(_ios
                                            , ipns
                                            , move(repo_root)
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
        , function<void(GenericConnection, asio::yield_context)> handler)
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(_ios);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

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
            fail(ec, "accept");
            if (!async_sleep(_ios, chrono::seconds(1), _shutdown_signal, yield)) {
                break;
            }
        } else {
            static const auto tcp_shutter = [](tcp::socket& s) {
                sys::error_code ec; // Don't throw
                s.shutdown(tcp::socket::shutdown_both, ec);
                s.close(ec);
            };

            GenericConnection connection(move(socket) , move(tcp_shutter));

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

#ifndef __ANDROID__
    auto pid_path = get_pid_path();
    if (exists(pid_path)) {
        throw runtime_error(util::str
             ( "[ABORT] Existing PID file ", pid_path
             , "; another client process may be running"
             , ", otherwise please remove the file."));
    }
    // Acquire a PID file for the life of the process
    assert(!_pid_file);
    _pid_file = make_unique<util::PidFile>(pid_path);
#endif

    auto ca_cert_path = _config.repo_root() / OUINET_CA_CERT_FILE;
    auto ca_key_path = _config.repo_root() / OUINET_CA_KEY_FILE;
    auto ca_dh_path = _config.repo_root() / OUINET_CA_DH_FILE;
    if (exists(ca_cert_path) && exists(ca_key_path) && exists(ca_dh_path)) {
        cout << "Loading existing CA certificate..." << endl;
        auto read_pem = [](auto path) {
            std::stringstream ss;
            ss << boost::filesystem::ifstream(path).rdbuf();
            return ss.str();
        };
        auto cert = read_pem(ca_cert_path);
        auto key = read_pem(ca_key_path);
        auto dh = read_pem(ca_dh_path);
        _ca_certificate = make_unique<CACertificate>(cert, key, dh);
    } else {
        cout << "Generating and storing CA certificate..." << endl;
        _ca_certificate = make_unique<CACertificate>();
        boost::filesystem::ofstream(ca_cert_path)
            << _ca_certificate->pem_certificate();
        boost::filesystem::ofstream(ca_key_path)
            << _ca_certificate->pem_private_key();
        boost::filesystem::ofstream(ca_dh_path)
            << _ca_certificate->pem_dh_param();
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
                          (GenericConnection c, asio::yield_context yield) {
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
                              (GenericConnection c, asio::yield_context yield) {
                        sys::error_code ec;
                        Request rq;
                        beast::flat_buffer buffer;
                        http::async_read(c, buffer, rq, yield[ec]);

                        if (ec) return;

                        auto rs = _front_end.serve( _config.injector_endpoint()
                                                  , rq
                                                  , _ipfs_cache.get()
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

        _injector->add(std::move(tcp_client));
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

boost::filesystem::path Client::get_pid_path() const
{
    return _state->get_pid_path();
}

//------------------------------------------------------------------------------
#ifndef __ANDROID__
int main(int argc, char* argv[])
{
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

        static auto pid_file_path = client.get_pid_path();
        // Force removal of PID file on abnormal exit
        std::atexit([] {
                if (!exists(pid_file_path)) return;
                cerr << "Warning: not a clean exit" << endl;
                remove(pid_file_path);
            });
    } catch (std::exception& e) {
        cerr << e.what() << endl;
        return 1;
    }

    ios.run();

    return EXIT_SUCCESS;
}
#endif
