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

#include "cache/bep5_http/client.h"

#include "namespaces.h"
#include "origin_pools.h"
#include "http_util.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_stream.h"
#include "util.h"
#include "async_sleep.h"
#include "endpoint.h"
#include "or_throw.h"
#include "request_routing.h"
#include "full_duplex_forward.h"
#include "client_config.h"
#include "client.h"
#include "authenticate.h"
#include "defer.h"
#include "default_timeout.h"
#include "constants.h"
#include "util/async_queue_reader.h"
#include "session.h"
#include "create_udp_multiplexer.h"
#include "ssl/ca_certificate.h"
#include "ssl/dummy_certificate.h"
#include "ssl/util.h"
#include "bittorrent/dht.h"
#include "bittorrent/mutable_data.h"

#ifndef __ANDROID__
#  include "force_exit_on_signal.h"
#endif // ifndef __ANDROID__

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/lampshade.h"
#include "ouiservice/pt-obfs2.h"
#include "ouiservice/pt-obfs3.h"
#include "ouiservice/pt-obfs4.h"
#include "ouiservice/tcp.h"
#include "ouiservice/utp.h"
#include "ouiservice/tls.h"
#include "ouiservice/weak_client.h"
#include "ouiservice/bep5/client.h"
#include "ouiservice/multi_utp_server.h"

#include "parse/number.h"
#include "util/signal.h"
#include "util/crypto.h"
#include "util/lru_cache.h"
#include "util/scheduler.h"
#include "util/reachability.h"
#include "upnp.h"
#include "util/handler_tracker.h"

#include "logger.h"

using namespace std;
using namespace ouinet;

namespace posix_time = boost::posix_time;
namespace bt = ouinet::bittorrent;

using tcp      = asio::ip::tcp;
using Request  = http::request<http::string_body>;
using Response = http::response<http::dynamic_body>;

static const fs::path OUINET_CA_CERT_FILE = "ssl-ca-cert.pem";
static const fs::path OUINET_CA_KEY_FILE = "ssl-ca-key.pem";
static const fs::path OUINET_CA_DH_FILE = "ssl-ca-dh.pem";

static bool log_transactions() {
    return logger.get_threshold() <= DEBUG
        || logger.get_log_file() != nullptr;
}

//------------------------------------------------------------------------------
struct UserAgentMetaData {
    boost::optional<bool> is_private;
    boost::optional<std::string> dht_group;

    static UserAgentMetaData extract(Request& rq) {
        UserAgentMetaData ret;

        {
            auto i = rq.find(http_::request_group_hdr);
            if (i != rq.end()) {
                ret.dht_group = i->value().to_string();
                rq.erase(i);
            }
        }
        {
            auto i = rq.find(http_::request_private_hdr);
            if (i != rq.end()) {
                ret.is_private = boost::iequals(i->value(), http_::request_private_true);
                rq.erase(i);
            }
        }

        return ret;
    }
};

//------------------------------------------------------------------------------
class Client::State : public enable_shared_from_this<Client::State> {
    friend class Client;

public:
    State(asio::io_context& ctx, ClientConfig cfg)
        : _ctx(ctx)
        , _config(move(cfg))
        // A certificate chain with OUINET_CA + SUBJECT_CERT
        // can be around 2 KiB, so this would be around 2 MiB.
        // TODO: Fine tune if necessary.
        , _ssl_certificate_cache(1000)
        , ssl_ctx{asio::ssl::context::tls_client}
        , inj_ctx{asio::ssl::context::tls_client}
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
        _bep5_http_cache = nullptr;
        _upnps.clear();
        _shutdown_signal();
        if (_injector) _injector->stop();
        if (_bt_dht) {
            _bt_dht->stop();
            _bt_dht = nullptr;
        }
        if (_udp_reachability) {
            _udp_reachability->stop();
            _udp_reachability = nullptr;
        }
    }

    void setup_cache();
    void set_injector(string);

    const asio_utp::udp_multiplexer& common_udp_multiplexer()
    {
        if (_udp_multiplexer) return *_udp_multiplexer;

        _udp_multiplexer
            = create_udp_multiplexer( _ctx
                                    , _config.repo_root() / "last_used_udp_port");

        _udp_reachability
            = make_unique<util::UdpServerReachabilityAnalysis>();
        _udp_reachability->start(get_executor(), *_udp_multiplexer);

        return *_udp_multiplexer;
    }

    std::shared_ptr<bt::MainlineDht> bittorrent_dht(asio::yield_context yield)
    {
        if (_bt_dht) return _bt_dht;

        auto bt_dht = make_shared<bt::MainlineDht>( _ctx.get_executor()
                                                  , _config.repo_root() / "dht");

        auto& mpl = common_udp_multiplexer();

        asio_utp::udp_multiplexer m(_ctx);
        sys::error_code ec;

        m.bind(mpl, ec);
        if (ec) return or_throw(yield, ec, _bt_dht);

        auto cc = _shutdown_signal.connect([&] { bt_dht.reset(); });

        auto ext_ep = bt_dht->set_endpoint(move(m), yield[ec]);
        if (ec) return or_throw(yield, ec, _bt_dht);

        setup_upnp(ext_ep.port(), mpl.local_endpoint());

        _bt_dht = move(bt_dht);
        return _bt_dht;
    }

private:
    GenericStream ssl_mitm_handshake( GenericStream&&
                                    , const Request&
                                    , asio::yield_context);

    void serve_request(GenericStream&& con, asio::yield_context yield);

    // All `fetch_*` functions below take care of keeping or dropping
    // Ouinet-specific internal HTTP headers as expected by upper layers.

    CacheEntry
    fetch_stored( const Request& request
                , request_route::Config& request_config
                , const std::string& dht_group
                , Cancel& cancel
                , Yield yield);

    Response fetch_fresh_from_front_end(const Request&, Yield);
    Session fetch_fresh_from_origin(const Request&, Yield);

    Session fetch_fresh_through_connect_proxy(const Request&, Cancel&, Yield);

    Session fetch_fresh_through_simple_proxy( Request
                                            , bool can_inject
                                            , Cancel& cancel
                                            , Yield);

    template<class Resp>
    void maybe_add_proto_version_warning(Resp& res) const {
        auto newest = newest_proto_seen;
        // Check if cache client knows about a newer protocol version too.
        auto c = get_cache();
        if (c && c->get_newest_proto_version() > newest)
            newest = c->get_newest_proto_version();
        if (newest > http_::protocol_version_current)
            res.set( http_::response_warning_hdr
                   , "Newer Ouinet protocol found in network, "
                     "please consider upgrading.");
    };

    CacheControl build_cache_control(request_route::Config& request_config);

    void listen_tcp( asio::yield_context
                   , tcp::endpoint
                   , const char* service
                   , function<void(GenericStream, asio::yield_context)>);

    void setup_injector(asio::yield_context);

    bool was_stopped() const {
        return _shutdown_signal.call_count() != 0;
    }

    fs::path ca_cert_path() const { return _config.repo_root() / OUINET_CA_CERT_FILE; }
    fs::path ca_key_path()  const { return _config.repo_root() / OUINET_CA_KEY_FILE;  }
    fs::path ca_dh_path()   const { return _config.repo_root() / OUINET_CA_DH_FILE;   }

    asio::io_context& get_io_context() { return _ctx; }
    asio::executor get_executor() { return _ctx.get_executor(); }

    Signal<void()>& get_shutdown_signal() { return _shutdown_signal; }

    bool maybe_handle_websocket_upgrade( GenericStream&
                                       , beast::string_view connect_host_port
                                       , Request&
                                       , Yield);
    void handle_retrieval_failure(GenericStream&, const Request&, Yield);

    GenericStream connect_to_origin(const Request&, Cancel&, Yield);

    unique_ptr<OuiServiceImplementationClient>
    maybe_wrap_tls(unique_ptr<OuiServiceImplementationClient>);

    cache::bep5_http::Client* get_cache() const { return _bep5_http_cache.get(); }

    void serve_utp_request(GenericStream, Yield);

    void setup_upnp(uint16_t ext_port, asio::ip::udp::endpoint local_ep) {
        if (_shutdown_signal) return;

        if (!local_ep.address().is_v4()) {
            LOG_WARN("Not setting up UPnP redirection because endpoint is not ipv4");
            return;
        }

        auto& p = _upnps[local_ep];

        if (p) {
            LOG_WARN("UPnP redirection for ", local_ep, " is already set");
            return;
        }

        p = make_unique<UPnPUpdater>(_ctx.get_executor(), ext_port, local_ep.port());
    }

    void idempotent_start_accepting_on_utp(asio::yield_context yield) {
        if (_multi_utp_server) return;
        assert(!_shutdown_signal);
        if (_shutdown_signal) return or_throw(yield, asio::error::operation_aborted);

        sys::error_code ec;
        auto dht = bittorrent_dht(yield[ec]);
        if (ec) return or_throw(yield, ec);

        auto exec = _ctx.get_executor();
        auto local_eps = dht->local_endpoints();

        _multi_utp_server
            = make_unique<ouiservice::MultiUtpServer>(exec, local_eps, nullptr);

        TRACK_SPAWN(_ctx, ([&, c = _shutdown_signal] (asio::yield_context yield) mutable {
            auto slot = c.connect([&] () mutable { _multi_utp_server = nullptr; });

            sys::error_code ec;
            _multi_utp_server->start_listen(yield[ec]);

            if (ec) {
                LOG_ERROR("Failed to start accepting on multi uTP service: ", ec.message());
                return;
            }

            while (!c) {
                sys::error_code ec;
                auto con = _multi_utp_server->accept(yield[ec]);
                if (c) return;
                if (ec == asio::error::operation_aborted) return;
                if (ec) {
                    LOG_WARN("Bep5Http: Failure to accept:", ec.message());
                    async_sleep(_ctx, 200ms, c, yield);
                    continue;
                }
                TRACK_SPAWN(_ctx, ([&, con = move(con)]
                                   (asio::yield_context yield) mutable {
                    Yield y(_ctx, yield, "uTPAccept");
                    serve_utp_request(move(con), y[ec]);
                }));
            }
        }));
    }

private:
    // The newest protocol version number seen in a trusted exchange
    // (i.e. from an injector exchange or injector-signed cached content).
    unsigned newest_proto_seen = http_::protocol_version_current;

    asio::io_context& _ctx;
    ClientConfig _config;
    std::unique_ptr<CACertificate> _ca_certificate;
    util::LruCache<string, string> _ssl_certificate_cache;
    std::unique_ptr<OuiServiceClient> _injector;
    std::unique_ptr<cache::bep5_http::Client> _bep5_http_cache;

    ClientFrontEnd _front_end;
    Signal<void()> _shutdown_signal;

    // For debugging
    uint64_t _next_connection_id = 0;
    ConnectionPool<Endpoint> _injector_connections;
    OriginPools _origin_pools;

    asio::ssl::context ssl_ctx;
    asio::ssl::context inj_ctx;

    boost::optional<asio::ip::udp::endpoint> _local_utp_endpoint;
    boost::optional<asio_utp::udp_multiplexer> _udp_multiplexer;
    unique_ptr<util::UdpServerReachabilityAnalysis> _udp_reachability;
    shared_ptr<bt::MainlineDht> _bt_dht;

    unique_ptr<ouiservice::MultiUtpServer> _multi_utp_server;
    shared_ptr<ouiservice::Bep5Client> _bep5_client;

    std::map<asio::ip::udp::endpoint, unique_ptr<UPnPUpdater>> _upnps;
};

//------------------------------------------------------------------------------
template<class Resp>
static
void handle_http_error( GenericStream& con
                      , Resp& res
                      , Yield yield)
{
    if (log_transactions()) {
        yield.log("=== Sending back response ===");
        yield.log(res);
    }

    http::async_write(con, res, yield);
}

template<class ReqBody>
static
void handle_bad_request( GenericStream& con
                       , const http::request<ReqBody>& req
                       , const string& message
                       , Yield yield)
{
    auto res = util::http_client_error(req, http::status::bad_request, "", message);
    auto yield_ = yield.tag("handle_bad_request");
    return handle_http_error(con, res, yield_);
}

//------------------------------------------------------------------------------
void
Client::State::serve_utp_request(GenericStream con, Yield yield)
{
    assert(_bep5_http_cache);
    Cancel cancel = _shutdown_signal;

    sys::error_code ec;

    http::request<http::empty_body> req;
    beast::flat_buffer buffer;

    {
        WatchDog watch_dog(_ctx , chrono::seconds(5) , [&] { con.close(); });

        http::async_read(con, buffer, req, yield[ec].tag("read"));

        if (!watch_dog.is_running()) {
            return or_throw(yield, asio::error::timed_out);
        }
    }

    if (ec || cancel) return;

    if (req.method() != http::verb::connect) {
        _bep5_http_cache->serve_local(req, con, cancel, yield[ec]);
        return;
    }

    // Connect to the injector and tunnel the transaction through it

    if (!_bep5_client) {
        return handle_bad_request(con, req, "No known injectors", yield[ec]);
    }

    auto inj = _bep5_client->connect( yield[ec].tag("connect_to_injector")
                                    , cancel
                                    , false
                                    , ouiservice::Bep5Client::injectors);

    if (cancel) ec = asio::error::operation_aborted;
    if (ec == asio::error::operation_aborted) return;
    if (ec) {
        ec = {};
        return handle_bad_request(con, req, "Failed to connect to injector", yield[ec]);
    }

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.prepare_payload();

    // No ``res.prepare_payload()`` since no payload is allowed for CONNECT:
    // <https://tools.ietf.org/html/rfc7231#section-6.3.1>.

    http::async_write(con, res, yield[ec].tag("write"));

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return;

    full_duplex(move(con), move(inj), yield[ec].tag("full_duplex"));
}

//------------------------------------------------------------------------------
CacheEntry
Client::State::fetch_stored( const Request& request
                           , request_route::Config& request_config
                           , const std::string& dht_group
                           , Cancel& cancel
                           , Yield yield)
{
    auto c = get_cache();

    const bool cache_is_disabled
        = !request_config.enable_stored
       || !c
       || !_config.is_cache_access_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheEntry>( yield
                                   , asio::error::operation_not_supported);
    }

    sys::error_code ec;

    auto key = key_from_http_req(request);
    assert(key);
    auto s = c->load(move(*key), dht_group, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, CacheEntry{});

    auto& hdr = s.response_header();

    if (!util::http_proto_version_check_trusted(hdr, newest_proto_seen))
        // The cached resource cannot be used, treat it like
        // not being found.
        return or_throw<CacheEntry>(yield, asio::error::not_found);

    auto tsh = util::http_injection_ts(hdr);
    auto ts = parse::number<time_t>(tsh);
    auto date = ( ts
                ? boost::posix_time::from_time_t(*ts)
                : boost::posix_time::not_a_date_time);

    maybe_add_proto_version_warning(hdr);
    assert(!hdr[http_::response_source_hdr].empty());  // for agent, set by cache
    return CacheEntry{date, move(s)};
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
                                         , _ctx.get_executor()
                                         , cancel
                                         , yield[ec]);

    if (ec) return or_throw<GenericStream>(yield, ec);

    auto sock = connect_to_host(lookup, _ctx.get_executor(), cancel, yield[ec]);

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
Response Client::State::fetch_fresh_from_front_end(const Request& rq, Yield yield)
{
    boost::optional<uint32_t> udp_port;

    if (_udp_multiplexer) {
        udp_port = _udp_multiplexer->local_endpoint().port();
    }

    auto res = _front_end.serve( _config
                               , rq
                               , _bep5_http_cache.get()
                               , *_ca_certificate
                               , udp_port
                               , _upnps
                               , _udp_reachability.get()
                               , yield.tag("serve_frontend"));

    res.set( http_::response_source_hdr  // for agent
           , http_::response_source_hdr_front_end);
    return res;
}

//------------------------------------------------------------------------------
Session Client::State::fetch_fresh_from_origin(const Request& rq, Yield yield)
{
    if (!_config.is_origin_access_enabled()) {
        return or_throw<Session>(yield, asio::error::operation_not_supported);
    }

    Cancel cancel;

    WatchDog watch_dog(_ctx
                      , default_timeout::fetch_http()
                      , [&] { cancel(); });

    sys::error_code ec;

    auto maybe_con = _origin_pools.get_connection(rq);
    OriginPools::Connection con;
    if (maybe_con) {
        con = std::move(*maybe_con);
    } else {
        auto stream = connect_to_origin(rq, cancel, yield[ec]);

        if (!ec && cancel) ec = asio::error::timed_out;
        if (ec) return or_throw<Session>(yield, ec);

        con = _origin_pools.wrap(rq, std::move(stream));
    }

    // Transform request from absolute-form to origin-form
    // https://tools.ietf.org/html/rfc7230#section-5.3
    auto rq_ = util::req_form_from_absolute_to_origin(rq);

    // Send request
    {
        auto con_close = cancel.connect([&] { con.close(); });
        http::async_write(con, rq_, yield[ec].tag("send-origin-request"));
    }

    if (ec) return or_throw<Session>(yield, ec);

    auto ret = Session::create(std::move(con), cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, Session());

    // Prevent others from inserting ouinet headers.
    util::remove_ouinet_fields_ref(ret.response_header());

    ret.response_header().set( http_::response_source_hdr  // for agent
                             , http_::response_source_hdr_origin);
    return ret;
}

//------------------------------------------------------------------------------
Session Client::State::fetch_fresh_through_connect_proxy( const Request& rq
                                                        , Cancel& cancel_
                                                        , Yield yield)
{
    // TODO: We're not re-using connections here. It's because the
    // ConnectionPool as it is right now can only work with http requests
    // and responses and thus can't be used for full-dupplex forwarding.

    Cancel cancel(cancel_);
    WatchDog watch_dog(_ctx, default_timeout::fetch_http(), [&]{ cancel(); });

    // Parse the URL to tell HTTP/HTTPS, host, port.
    util::url_match url;

    if (!match_http_url(rq.target(), url)) {
        // unsupported URL
        return or_throw<Session>(yield, asio::error::operation_not_supported);
    }

    // Connect to the injector/proxy.
    sys::error_code ec;

    auto inj = _injector->connect(yield[ec].tag("connect_to_injector"), cancel);

    if (ec) return or_throw<Session>(yield, ec);

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
                        ( _ctx.get_executor()
                        , inj.connection
                        , connreq
                        , default_timeout::fetch_http()
                        , cancel
                        , yield[ec].tag("connreq"));

    if (connres.result() != http::status::ok) {
        // This error code is quite fake, so log the error too.
        // Unfortunately there is no body to show.
        yield.tag("proxy_connect").log(connres);
        return or_throw<Session>(yield, asio::error::connection_refused);
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

    if (ec) return or_throw<Session>(yield, ec);

    // TODO: move
    auto rq_ = util::req_form_from_absolute_to_origin(rq);

    {
        auto slot = cancel.connect([&con] { con.close(); });
        http::async_write(con, rq_, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, Session());
    }

    auto session = Session::create(move(con), cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, Session());

    // Prevent others from inserting ouinet headers.
    util::remove_ouinet_fields_ref(session.response_header());

    session.response_header().set( http_::response_source_hdr  // for agent
                                 , http_::response_source_hdr_proxy);
    return session;
}
//------------------------------------------------------------------------------
Session Client::State::fetch_fresh_through_simple_proxy
        ( Request request
        , bool can_inject
        , Cancel& cancel
        , Yield yield)
{
    sys::error_code ec;

    // Connect to the injector.
    ConnectionPool<Endpoint>::Connection con;
    if (_injector_connections.empty()) {
        if (log_transactions()) {
            yield.log("Connecting to the injector");
        }

        auto c = _injector->connect(yield[ec].tag("connect_to_injector2"), cancel);

        assert(!cancel || ec == asio::error::operation_aborted);

        if (ec) {
            if (log_transactions()) {
                yield.log("Failed to connect to injector ec:", ec.message());
            }
            return or_throw<Session>(yield, ec);
        }

        assert(c.connection.has_implementation());

        con = _injector_connections.wrap(std::move(c.connection));
        *con = c.remote_endpoint;
    } else {
        if (log_transactions()) {
            yield.log("Reusing existing injector connection");
        }

        con = _injector_connections.pop_front();
    }

    auto cancel_slot = cancel.connect([&] {
        con.close();
    });

    // Build the actual request to send to the injector.
    if (auto credentials = _config.credentials_for(*con))
        request = authorize(request, *credentials);

    if (can_inject) {
        bool keepalive = request.keep_alive();
        request = util::to_injector_request(move(request));
        request.keep_alive(keepalive);
    }

    if (log_transactions()) {
        yield.log("Sending a request to the injector");
    }
    // Send request
    http::async_write(con, request, yield[ec].tag("inj-request"));

    if (!ec && cancel_slot) {
        ec = asio::error::operation_aborted;
    }

    if (ec && log_transactions()) {
        yield.log("Failed to send request to the injector");
    }

    if (ec) return or_throw<Session>(yield, ec);

    if (log_transactions()) {
        yield.log("Reading response");
    }

    cancel_slot = {};

    // Receive response
    auto session = Session::create(move(con), cancel, yield[ec]);

    auto& hdr = session.response_header();

    if (cancel)
        ec = asio::error::operation_aborted;
    else if ( !ec
            && can_inject
            && !util::http_proto_version_check_trusted(hdr, newest_proto_seen))
        // The injector using an unacceptable protocol version is treated like
        // the Injector mechanism being disabled.
        ec = asio::error::operation_not_supported;

    if (log_transactions()) {
        yield.log("End reading response. ec:", ec.message());
    }

    if (ec) return or_throw(yield, ec, std::move(session));

    // Store keep-alive connections in connection pool

    if (can_inject) {
        maybe_add_proto_version_warning(hdr);

        session.response_header().set( http_::response_source_hdr  // for agent
                                     , http_::response_source_hdr_injector);
    } else {
        // Prevent others from inserting ouinet headers.
        util::remove_ouinet_fields_ref(hdr);

        session.response_header().set( http_::response_source_hdr  // for agent
                                     , http_::response_source_hdr_proxy);
    }
    return session;
}

//------------------------------------------------------------------------------
class Client::ClientCacheControl {
public:
    ClientCacheControl( Client::State& client_state
                      , request_route::Config& request_config)
        : client_state(client_state)
        , request_config(request_config)
        , cc(client_state.get_executor(), OUINET_CLIENT_SERVER_STRING)
    {
        cc.fetch_fresh = [&] (const Request& rq, Cancel& cancel, Yield yield) {
            return fetch_fresh(rq, cancel, yield.tag("fresh"));
        };

        cc.fetch_stored = [&] (const Request& rq, const std::string& dht_group, Cancel& cancel, Yield yield) {
            return fetch_stored(rq, dht_group, cancel, yield.tag("cache"));
        };

        cc.max_cached_age(client_state._config.max_cached_age());
    }

    Session fetch_fresh(const Request& request, Cancel& cancel, Yield yield) {
        namespace err = asio::error;

        if (log_transactions()) {
            yield.log("start");
        }

        if (!client_state._config.is_injector_access_enabled()) {
            if (log_transactions()) {
                yield.log("disabled");
            }

            return or_throw<Session>(yield, err::operation_not_supported);
        }

        sys::error_code ec;
        auto s = client_state.fetch_fresh_through_simple_proxy( request
                                                              , true
                                                              , cancel
                                                              , yield[ec]);

        if (log_transactions()) {
            if (!ec) {
                yield.log("finish: ", ec.message(), ", status: ", s.response_header().result());
            } else {
                yield.log("finish: ", ec.message());
            }
        }

        return or_throw(yield, ec, move(s));
    }

    CacheEntry
    fetch_stored(const Request& request, const std::string& dht_group, Cancel& cancel, Yield yield) {
        if (log_transactions()) {
            yield.log("start");
        }

        sys::error_code ec;
        auto r = client_state.fetch_stored( request
                                          , request_config
                                          , dht_group
                                          , cancel
                                          , yield[ec]);

        if (log_transactions()) {
            yield.log("finish: ", ec.message(), " canceled: ", bool(cancel));
        }

        return or_throw(yield, ec, move(r));
    }

    // Closes `con` when it can no longer be used.
    // If an error is reported and it is still open,
    // a response may still be sent to it.
    // The return value indicates whether the connection
    // should be kept alive afterwards.
    bool fetch( GenericStream& con
              , const Request& rq
              , const UserAgentMetaData& meta
              , Yield yield)
    {
        namespace err = asio::error;
        using request_route::fresh_channel;

        sys::error_code last_error = err::operation_not_supported;

        while (!request_config.fresh_channels.empty()) {
            if (client_state._shutdown_signal)
                return or_throw(yield, err::operation_aborted, false);

            auto r = request_config.fresh_channels.front();
            request_config.fresh_channels.pop();

            Cancel cancel(client_state._shutdown_signal);

            auto& ctx = client_state.get_io_context();

            WatchDog wd(ctx, chrono::minutes(3), [&] { cancel(); });

            sys::error_code ec;

            switch (r) {
                case fresh_channel::_front_end: {
                    Response res = client_state.fetch_fresh_from_front_end(rq, yield);
                    res.keep_alive(rq.keep_alive());
                    http::async_write(con, res, asio::yield_context(yield)[ec]);

                    bool keep_alive = !ec && rq.keep_alive();
                    if (!keep_alive) con.close();
                    return or_throw(yield, ec, keep_alive);
                }
                case fresh_channel::secure_origin:
                case fresh_channel::origin: {
                    auto rq_ = rq;

                    auto y = yield.tag("origin");

                    if (log_transactions()) {
                        y.log("start");
                    }

                    if (r == fresh_channel::secure_origin
                            && rq_.target().starts_with("http://")) {
                        auto target = rq_.target().to_string();
                        target.insert(4, "s"); // http:// -> https://
                        rq_.target(move(target));
                    }

                    auto session = client_state.fetch_fresh_from_origin(rq_, y[ec]);

                    if (log_transactions()) {
                        y.log("fetch: ", ec.message());
                    }

                    if (ec) break;

                    session.flush_response(con, cancel, y[ec]);

                    if (log_transactions()) {
                        y.log("flush: ", ec.message());
                    }

                    bool keep_alive = !ec && rq_.keep_alive() && session.keep_alive();
                    if (!keep_alive) {
                        session.close();
                        con.close();
                    }
                    return or_throw(y, ec, keep_alive);
                }
                case fresh_channel::proxy: {
                    auto y = yield.tag("proxy");

                    if (log_transactions()) {
                        y.log("start");
                    }

                    if (!client_state._config.is_proxy_access_enabled()) {
                        if (log_transactions()) {
                            y.log("disabled");
                        }
                        continue;
                    }

                    Session session;

                    if (rq.target().starts_with("https://")) {
                        session = client_state.fetch_fresh_through_connect_proxy
                                (rq, cancel, y[ec]);
                    }
                    else {
                        session = client_state.fetch_fresh_through_simple_proxy
                                (rq, false, cancel, y[ec]);
                    }

                    if (log_transactions()) {
                        y.log("Proxy fetch: ", ec.message());
                    }

                    if (ec) break;

                    session.flush_response(con, cancel, y[ec]);

                    if (log_transactions()) {
                        y.log("flush: ", ec.message());
                    }

                    bool keep_alive = !ec && rq.keep_alive() && session.keep_alive();
                    if (!keep_alive) {
                        session.close();
                        con.close();
                    }
                    return or_throw(yield, ec, keep_alive);
                }
                case fresh_channel::injector: {
                    auto y = yield.tag("injector");

                    sys::error_code fresh_ec;
                    sys::error_code cache_ec;

                    if (log_transactions()) {
                        y.log("start");
                    }

                    auto s = cc.fetch(rq, meta.dht_group, fresh_ec, cache_ec, cancel, y[ec]);

                    if (log_transactions()) {
                        y.log("cc.fetch ec:", ec.message(),
                              " fresh_ec:", fresh_ec.message(),
                              " cache_ec:", cache_ec.message());
                    }

                    if (ec) break;

                    auto& rsh = s.response_header();

                    if (log_transactions()) {
                        y.log("Response header:");
                        y.log(rsh);
                    }

                    assert(!fresh_ec || !cache_ec); // At least one success
                    assert( fresh_ec ||  cache_ec); // One needs to fail

                    auto injector_error = rsh[http_::response_error_hdr];
                    if (!injector_error.empty()) {
                        if (log_transactions()) {
                            y.log("Error from injector: ", injector_error);
                        }
                        ec = asio::error::invalid_argument;
                        break;
                    }

                    auto exec = ctx.get_executor();

                    using http_response::Part;

                    util::AsyncQueue<boost::optional<Part>> qst(exec), qag(exec); // to storage, agent

                    WaitCondition wc(ctx);

                    auto cache = client_state.get_cache();

                    bool do_cache =
                        ( cache
                        && rsh[http_::response_source_hdr] != http_::response_source_hdr_local_cache
                        && CacheControl::ok_to_cache(rq, rsh)
                        && meta.dht_group);

                    if (do_cache) {
                        TRACK_SPAWN(ctx, ([
                            &, cache = std::move(cache),
                            lock = wc.lock()
                        ] (asio::yield_context yield_) {
                            auto key = key_from_http_req(rq); assert(key);
                            AsyncQueueReader rr(qst);
                            sys::error_code ec;
                            auto y_ = y.detach(yield_);
                            cache->store(*key, *meta.dht_group, rr, cancel, y_[ec]);
                        }));
                    }

                    TRACK_SPAWN(ctx, ([
                        &,
                        lock = wc.lock()
                    ] (asio::yield_context yield_) {
                        sys::error_code ec;
                        auto rr = std::make_unique<AsyncQueueReader>(qag);
                        Session sag = Session::create_from_reader(std::move(rr), cancel, yield_[ec]);
                        if (!ec) sag.flush_response(con, cancel, yield_[ec]);
                    }));

                    s.flush_response(cancel, yield[ec],
                        [&] ( Part&& part
                            , Cancel& cancel
                            , asio::yield_context yield)
                        {
                            if (do_cache) qst.push_back(part);
                            qag.push_back(std::move(part));
                        });

                    if (do_cache) qst.push_back(boost::none);
                    qag.push_back(boost::none);

                    wc.wait(yield);

                    bool keep_alive = !ec && rq.keep_alive() && s.keep_alive();
                    if (!keep_alive) {
                        s.close();
                        con.close();
                    }

                    if (log_transactions()) {
                        y.log("finish: ", ec.message());
                    }

                    return or_throw(yield, ec, keep_alive);
                }
            }

            if (cancel) ec = err::timed_out;
            last_error = ec;
        }

        assert(last_error);
        return or_throw(yield, last_error, rq.keep_alive());
    }

private:
    Client::State& client_state;
    request_route::Config& request_config;
    CacheControl cc;
};

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
            sys::error_code ec_;
            handle_bad_request(browser, rq, "Not a websocket server", yield[ec_]);
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
void Client::State::handle_retrieval_failure( GenericStream& con
                                            , const Request& req
                                            , Yield yield)
{
    auto res = util::http_client_error
        ( req, http::status::bad_gateway, http_::response_error_hdr_retrieval_failed
        , "Failed to retrieve the resource "
          "(after attempting all configured mechanisms)");
    maybe_add_proto_version_warning(res);
    auto yield_ = yield.tag("handle_retrieval_failure");
    return handle_http_error(con, res, yield_);
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

    // For use with non-tls (http://) sites
    const rr::Config secure_first_config
        { true
        , queue<fresh_channel>({ fresh_channel::secure_origin
                               , fresh_channel::injector
                               , fresh_channel::proxy
                               , fresh_channel::origin})};

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
    auto x_private_getter([](const Request& r) {return r[http_::request_private_hdr];});
    auto target_getter([](const Request& r) {return r.target();});

    auto local_rx = util::str("https?://[^:/]+\\.", _config.local_domain(), "(:[0-9]+)?/.*");

    const vector<Match> matches({
        // Handle requests to <http://localhost/> internally.
        Match( reqexpr::from_regex(host_getter, "localhost")
             , {false, queue<fresh_channel>({fresh_channel::_front_end})} ),

        Match( reqexpr::from_regex(x_oui_dest_getter, "OuiClient")
             , {false, queue<fresh_channel>({fresh_channel::_front_end})} ),

        // Access to sites under the local TLD are always accessible
        // with good connectivity, so always use the Origin channel
        // and never cache them.
        Match( reqexpr::from_regex(target_getter, local_rx)
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

        Match( reqexpr::from_regex(x_private_getter, "True")
             , nocache_request_config),

        // Disable cache and always go to origin for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://ident\\.me/.*")
        //     , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        // Disable cache and always go to origin for these google sites.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?google\\.com/complete/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https://safebrowsing\\.googleapis\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?google-analytics\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        // Disable cache and always go to origin for these mozilla sites.
        Match( reqexpr::from_regex(target_getter, "https?://content-signature\\.cdn\\.mozilla\\.net/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*services\\.mozilla\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://services\\.addons\\.mozilla\\.org/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://versioncheck-bg\\.addons\\.mozilla\\.org/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*cdn\\.mozilla\\.net/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://detectportal\\.firefox\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        // Ads
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*googlesyndication\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*googletagservices\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*moatads\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*amazon-adsystem\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*adsafeprotected\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*ads-twitter\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*doubleclick\\.net/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*summerhamster\\.com/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        Match( reqexpr::from_regex(target_getter, "https?://ping.chartbeat.net/.*")
             , {false, queue<fresh_channel>({fresh_channel::origin})} ),

        // Disable cache and always go to proxy for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://ifconfig\\.co/.*")
        //     , {false, queue<fresh_channel>({fresh_channel::proxy})} ),
        // Force cache and default channels for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example\\.com/.*")
        //     , {true, queue<fresh_channel>()} ),
        // Force cache and particular channels for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example\\.net/.*")
        //     , {true, queue<fresh_channel>({fresh_channel::injector})} ),

        Match(reqexpr::from_regex(target_getter, "http://.*"), secure_first_config)
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

        Yield yield(_ctx.get_executor(), yield_, util::str('C', connection_id));

        if ( ec == http::error::end_of_stream
          || ec == asio::ssl::error::stream_truncated) break;

        if (ec) {
            if (ec != asio::error::operation_aborted) {
                LOG_WARN("Failed to read request: ", ec.message());
            }
            return;
        }

        Request req(reqhp.release());

        if (!authenticate(req, con, _config.client_credentials(), yield[ec].tag("auth"))) {
            continue;
        }

        if (log_transactions()) {
            yield.log("=== New request ===");
            yield.log(req.base());
            auto on_exit = defer([&] {
                yield.log("Done");
            });
        }

        auto target = req.target();

        // Perform MitM for CONNECT requests (to be able to see encrypted requests)
        if (!mitm && req.method() == http::verb::connect) {
            sys::error_code ec;
            // Subsequent access to the connection will use the encrypted channel.
            con = ssl_mitm_handshake(move(con), req, yield[ec].tag("mitm_hanshake"));
            if (ec) {
                if (log_transactions()) {
                    yield.log("Mitm exception: ", ec.message());
                }
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
                sys::error_code ec_;
                handle_bad_request(con, req, "Not a proxy request", yield[ec_]);
                if (req.keep_alive()) continue;
                else return;
            }
        }

        request_config = route_choose_config(req, matches, default_request_config);

        auto meta = UserAgentMetaData::extract(req);

        bool keep_alive
            = cache_control.fetch(con, req, meta, yield[ec].tag("fetch"));

        if (ec) {
            if (log_transactions()) {
                yield.log("error writing back response: ", ec.message());
            }
            if (con.is_open()) {
                sys::error_code ec_;
                handle_retrieval_failure(con, req, yield[ec_]);
                if (ec_) keep_alive = false;
            }
        }

        if (!keep_alive) {
            con.close();
            break;
        }
    }
}

//------------------------------------------------------------------------------
void Client::State::setup_cache()
{
    if (_config.cache_type() == ClientConfig::CacheType::Bep5Http) {
        Cancel cancel = _shutdown_signal;

        TRACK_SPAWN(_ctx, ([
            this,
            cancel = move(cancel)
        ] (asio::yield_context yield) {
            if (cancel) return;
            LOG_DEBUG("HTTP signing public key (Ed25519): ", _config.cache_http_pub_key());

            sys::error_code ec;

            auto dht = bittorrent_dht(yield[ec]);
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    LOG_ERROR("Failed to initialize BT DHT ", ec.message());
                }
                return;
            }

            assert(!_shutdown_signal || ec == asio::error::operation_aborted);

            _bep5_http_cache
                = cache::bep5_http::Client::build( dht
                                                 , *_config.cache_http_pub_key()
                                                 , _config.repo_root()/"bep5_http"
                                                 , _config.max_cached_age()
                                                 , logger.get_threshold()
                                                 , yield[ec]);

            if (cancel) ec = asio::error::operation_aborted;
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    LOG_ERROR("Failed to initialize cache::bep5_http::Client: "
                             , ec.message());
                }
                return;
            }

            idempotent_start_accepting_on_utp(yield[ec]);

            if (ec) {
                LOG_ERROR("Failed to start accepting on uTP ", ec.message());
            }
        }));
    }
}

//------------------------------------------------------------------------------
void Client::State::listen_tcp
        ( asio::yield_context yield
        , tcp::endpoint local_endpoint
        , const char* service
        , function<void(GenericStream, asio::yield_context)> handler)
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(_ctx);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) {
        LOG_ERROR("Failed to open tcp acceptor for service \"",service,"\": ", ec.message());
        return;
    }

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) {
        LOG_ERROR("Failed to bind tcp acceptor for service \"", service,"\": "
                 , ec.message());
        return;
    }

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) {
        LOG_ERROR("Failed to 'listen' to service \"", service, "\""
                  " on tcp acceptor: ", ec.message());
        return;
    }

    auto shutdown_acceptor_slot = _shutdown_signal.connect([&acceptor] {
        acceptor.close();
    });

    LOG_INFO("Client accepting to ", service, " on TCP:", acceptor.local_endpoint());

    WaitCondition wait_condition(_ctx);

    for(;;)
    {
        tcp::socket socket(_ctx);
        acceptor.async_accept(socket, yield[ec]);

        if(ec) {
            if (ec == asio::error::operation_aborted) break;

            LOG_WARN("Accept failed on tcp acceptor service \"",service,"\": ", ec.message());

            if (!async_sleep(_ctx, chrono::seconds(1), _shutdown_signal, yield)) {
                break;
            }
        } else {
            static const auto tcp_shutter = [](tcp::socket& s) {
                sys::error_code ec; // Don't throw
                s.shutdown(tcp::socket::shutdown_both, ec);
                s.close(ec);
            };

            GenericStream connection(move(socket) , move(tcp_shutter));

            // Increase the size of the coroutine stack.
            // Some interesing info:
            // https://lists.ceph.io/hyperkitty/list/dev@ceph.io/thread/6LBFZIFUPTJQ3SNTLVKSQMVITJWVWTZ6/
            boost::coroutines::attributes attribs;
            attribs.size *= 2;

            TRACK_SPAWN( _ctx, ([
                this,
                self = shared_from_this(),
                c = move(connection),
                handler,
                lock = wait_condition.lock()
            ](asio::yield_context yield) mutable {
                if (was_stopped()) return;
                handler(move(c), yield);
            }), attribs);
        }
    }

    wait_condition.wait(yield);
}

//------------------------------------------------------------------------------
void Client::State::start()
{
    ssl::util::load_tls_ca_certificates(ssl_ctx, _config.tls_ca_cert_store_path());

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

    TRACK_SPAWN(_ctx, ([
        this,
        self = shared_from_this()
    ] (asio::yield_context yield) {
        if (was_stopped()) return;

        sys::error_code ec;

        setup_injector(yield[ec]);

        if (was_stopped()) return;

        if (ec) {
            LOG_ERROR("Failed to setup injector: ", ec.message());
        }

        setup_cache();

        listen_tcp( yield[ec]
                  , _config.local_endpoint()
                  , "browser requests"
                  , [this, self]
                    (GenericStream c, asio::yield_context yield) {
                serve_request(move(c), yield);
            });
    }));

    if (_config.front_end_endpoint() != tcp::endpoint()) {
        TRACK_SPAWN( _ctx, ([
            this,
            self = shared_from_this()
        ] (asio::yield_context yield) {
            if (was_stopped()) return;

            sys::error_code ec;

            auto ep = _config.front_end_endpoint();
            if (ep == tcp::endpoint()) return;

            LOG_INFO("Serving front end on ", ep);

            listen_tcp( yield[ec]
                      , ep
                      , "frontend"
                      , [this, self]
                        (GenericStream c, asio::yield_context yield_) {
                  Yield yield(_ctx, yield_, "Frontend");
                  sys::error_code ec;
                  Request rq;
                  beast::flat_buffer buffer;
                  http::async_read(c, buffer, rq, yield[ec]);

                  if (ec) return;

                  auto rs = fetch_fresh_from_front_end(rq, yield[ec]);

                  if (ec) return;

                  http::async_write(c, rs, yield[ec]);
            });
        }));
    }
}

//------------------------------------------------------------------------------
unique_ptr<OuiServiceImplementationClient>
Client::State::maybe_wrap_tls(unique_ptr<OuiServiceImplementationClient> client)
{
    bool enable_injector_tls = !_config.tls_injector_cert_path().empty();

    if (!enable_injector_tls) {
        LOG_WARN("Connection to the injector shall not be encrypted");
        return client;
    }

    return make_unique<ouiservice::TlsOuiServiceClient>(move(client), inj_ctx);
}

void Client::State::setup_injector(asio::yield_context yield)
{
    _injector = std::make_unique<OuiServiceClient>(_ctx.get_executor());

    auto injector_ep = _config.injector_endpoint();

    if (!injector_ep) return;

    LOG_INFO("Setting up injector: ", *injector_ep);

    std::unique_ptr<OuiServiceImplementationClient> client;

    if (injector_ep->type == Endpoint::I2pEndpoint) {
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((_config.repo_root()/"i2p").string(), _ctx.get_executor());
        auto i2p_client = i2p_service->build_client(injector_ep->endpoint_string);

        /*
        if (!i2p_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        */
        client = std::move(i2p_client);
    } else if (injector_ep->type == Endpoint::TcpEndpoint) {
        auto tcp_client = make_unique<ouiservice::TcpOuiServiceClient>(_ctx.get_executor(), injector_ep->endpoint_string);

        if (!tcp_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = maybe_wrap_tls(move(tcp_client));
    } else if (injector_ep->type == Endpoint::UtpEndpoint) {
        sys::error_code ec;
        asio_utp::udp_multiplexer m(_ctx);
        m.bind(common_udp_multiplexer(), ec);
        assert(!ec);

        auto utp_client = make_unique<ouiservice::UtpOuiServiceClient>
            (_ctx.get_executor(), move(m), injector_ep->endpoint_string);

        if (!utp_client->verify_remote_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }

        client = maybe_wrap_tls(move(utp_client));
    } else if (injector_ep->type == Endpoint::Bep5Endpoint) {

        sys::error_code ec;
        auto dht = bittorrent_dht(yield[ec]);
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                LOG_ERROR("Failed to set up Bep5Client at setting up BT DHT ", ec.message());
            }
            return or_throw(yield, ec);
        }

        _bep5_client = make_shared<ouiservice::Bep5Client>
            ( dht
            , injector_ep->endpoint_string
            , injector_helpers_swarm_name
            , &inj_ctx);

        client = make_unique<ouiservice::WeakOuiServiceClient>(_bep5_client);

        idempotent_start_accepting_on_utp(yield[ec]);

        if (ec) {
            LOG_ERROR("Failed to start accepting on uTP ", ec.message());
        }
/*
    } else if (injector_ep->type == Endpoint::LampshadeEndpoint) {
        auto lampshade_client = make_unique<ouiservice::LampshadeOuiServiceClient>(_ctx, injector_ep->endpoint_string);

        if (!lampshade_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(lampshade_client);
*/
    } else if (injector_ep->type == Endpoint::Obfs2Endpoint) {
        auto obfs2_client = make_unique<ouiservice::Obfs2OuiServiceClient>(_ctx, injector_ep->endpoint_string, _config.repo_root()/"obfs2-client");

        if (!obfs2_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(obfs2_client);
    } else if (injector_ep->type == Endpoint::Obfs3Endpoint) {
        auto obfs3_client = make_unique<ouiservice::Obfs3OuiServiceClient>(_ctx, injector_ep->endpoint_string, _config.repo_root()/"obfs3-client");

        if (!obfs3_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(obfs3_client);
    } else if (injector_ep->type == Endpoint::Obfs4Endpoint) {
        auto obfs4_client = make_unique<ouiservice::Obfs4OuiServiceClient>(_ctx, injector_ep->endpoint_string, _config.repo_root()/"obfs4-client");

        if (!obfs4_client->verify_endpoint()) {
            return or_throw(yield, asio::error::invalid_argument);
        }
        client = std::move(obfs4_client);
    }

    _injector->add(*injector_ep, std::move(client));

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
        LOG_ERROR("Failed to parse endpoint \"", injector_ep_str, "\"");
        return;
    }

    auto current_ep = _config.injector_endpoint();

    if (current_ep && *injector_ep == *current_ep) {
        return;
    }

    _config.set_injector_endpoint(*injector_ep);

    TRACK_SPAWN(_ctx, ([self = shared_from_this(), injector_ep_str] (auto yield) {
            if (self->was_stopped()) return;
            sys::error_code ec;
            self->setup_injector(yield[ec]);

            if (ec == asio::error::invalid_argument) {
                LOG_ERROR("Failed to parse endpoint \"", injector_ep_str, "\"");
            }
        }));
}

//------------------------------------------------------------------------------
Client::Client(asio::io_context& ctx, ClientConfig cfg)
    : _state(make_shared<State>(ctx, move(cfg)))
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

void Client::set_credentials(const char* injector_ep, const char* cred)
{
    auto opt_ep = parse_endpoint(injector_ep);
    if (!opt_ep) {
        LOG_ERROR("Client::set_credentials: Failed to parse endpoint:", injector_ep);
        return;
    }
    _state->_config.set_credentials(*opt_ep, cred);
}

void Client::charging_state_change(bool is_charging) {
    LOG_DEBUG("Charging state changed, is charging: ", is_charging);
    //TODO(peter) do something
}

void Client::wifi_state_change(bool is_wifi_connected) {
    LOG_DEBUG("Wifi state changed, is connected: ", is_wifi_connected);
    //TODO(peter) do something
}

fs::path Client::ca_cert_path() const
{
    return _state->ca_cert_path();
}

fs::path Client::get_or_gen_ca_root_cert(const string repo_root)
{
    fs::path repo_path = fs::path(repo_root);
    fs::path ca_cert_path = repo_root / OUINET_CA_CERT_FILE;
    fs::path ca_key_path = repo_root / OUINET_CA_KEY_FILE;
    fs::path ca_dh_path = repo_root / OUINET_CA_DH_FILE;
    get_or_gen_tls_cert<CACertificate>
        ( "Your own local Ouinet client"
        , ca_cert_path, ca_key_path, ca_dh_path);
    return ca_cert_path;
}

//------------------------------------------------------------------------------
#ifndef __ANDROID__
int main(int argc, char* argv[])
{
    util::crypto_init();

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

    asio::io_context ctx;

    asio::signal_set signals(ctx, SIGINT, SIGTERM);

    Client client(ctx, move(cfg));

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&client, &signals, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            LOG_INFO("GOT SIGNAL ", signal_number);
            HandlerTracker::stopped();
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

    ctx.run();

    LOG_INFO("Exiting gracefuly");

    return EXIT_SUCCESS;
}
#endif
