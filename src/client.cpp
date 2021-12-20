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
#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/indexed.hpp>
#include <boost/regex.hpp>
#include <iterator>
#include <iostream>
#include <cstdlib>  // for atexit()

#include "cache/client.h"

#include "namespaces.h"
#include "origin_pools.h"
#include "doh.h"
#include "http_util.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_stream.h"
#include "util.h"
#include "async_sleep.h"
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
#include "util/queue_reader.h"
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
#include "util/async_job.h"
#include "upnp.h"
#include "util/handler_tracker.h"

#include "logger.h"

#define _YDEBUG(y, ...) do { if (logger.get_threshold() <= DEBUG) y.log(DEBUG, __VA_ARGS__); } while (false)
#define _YWARN(y, ...) do { if (logger.get_threshold() <= WARN) y.log(WARN, __VA_ARGS__); } while (false)
#define _YERROR(y, ...) do { if (logger.get_threshold() <= ERROR) y.log(ERROR, __VA_ARGS__); } while (false)

using namespace std;
using namespace ouinet;

namespace posix_time = boost::posix_time;
namespace bt = ouinet::bittorrent;

using tcp      = asio::ip::tcp;
using Request  = http::request<http::string_body>;
using Response = http::response<http::dynamic_body>;
using TcpLookup = tcp::resolver::results_type;

static const fs::path OUINET_CA_CERT_FILE = "ssl-ca-cert.pem";
static const fs::path OUINET_CA_KEY_FILE = "ssl-ca-key.pem";
static const fs::path OUINET_CA_DH_FILE = "ssl-ca-dh.pem";

// Flags for normal, case-insensitive regular expression.
static const auto rx_icase = boost::regex::normal | boost::regex::icase;

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

    // Apply the metadata to the given request.
    template<class Req>
    void apply_to(Req& rq) const {
        if (is_private && *is_private)
            rq.set(http_::request_private_hdr, http_::request_private_true);
        if (dht_group)
            rq.set(http_::request_group_hdr, *dht_group);
    }
};

//------------------------------------------------------------------------------
class Client::State : public enable_shared_from_this<Client::State> {
    friend class Client;

    enum class InternalState {
        Created, Failed, Started, Stopped
    };

public:
    State(asio::io_context& ctx, ClientConfig cfg)
        : _ctx(ctx)
        , _config(move(cfg))
        // A certificate chain with OUINET_CA + SUBJECT_CERT
        // can be around 2 KiB, so this would be around 2 MiB.
        // TODO: Fine tune if necessary.
        , _ssl_certificate_cache(1000)
        , _injector_starting{get_executor()}
        , _cache_starting{get_executor()}
        , ssl_ctx{asio::ssl::context::tls_client}
        , inj_ctx{asio::ssl::context::tls_client}
        , _bt_dht_wc(_ctx)
        , _multi_utp_server_wc(_ctx)
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
        if (_internal_state == InternalState::Created)
            _internal_state = InternalState::Stopped;

        if (_internal_state != InternalState::Started)
            return;

        _internal_state = InternalState::Stopped;

        // Requests waiting for these after stop may get "operation aborted"
        // when these are destroyed.
        // If the cancellation signal in `wait_for_*` was not called,
        // `return_or_throw_on_error` would catch this and trigger an assertion error.
        // Since requests waiting for these after stop should not happen,
        // these are not reset here, as we do want that crash when debugging.
        if (_injector_starting) _injector_starting->notify(asio::error::shut_down);
        if (_cache_starting) _cache_starting->notify(asio::error::shut_down);

        _cache = nullptr;
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

    Client::RunningState get_state() const noexcept {
        switch (_internal_state) {
        case InternalState::Created:
            return Client::RunningState::Created;
        case InternalState::Failed:
            return Client::RunningState::Failed;
        case InternalState::Started:
            break;  // handled below
        case InternalState::Stopped:
            // TODO: Gather stopped state from members
            // instead of checking that all tasks in the context
            // (even those which are not part of the client object) are finished.
            return _ctx.stopped()
                ? Client::RunningState::Stopped
                : Client::RunningState::Stopping;
        }
        assert(_internal_state == InternalState::Started);

        if (was_stopped())
            return Client::RunningState::Stopping;  // `stop()` did not run yet

        // TODO: check proxy acceptor
        // TODO: check front-end acceptor
        bool use_injector(_config.injector_endpoint());
        bool use_cache(_config.cache_type() != ClientConfig::CacheType::None);
        if (use_injector && _injector_starting)
            return Client::RunningState::Starting;
        if (use_cache && _cache_starting)
            return Client::RunningState::Starting;
        if (use_injector && _injector_start_ec)
            return Client::RunningState::Degraded;
        if (use_cache && _cache_start_ec)
            return Client::RunningState::Degraded;

        return Client::RunningState::Started;
    }

    void setup_cache(asio::yield_context);

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

        // Ensure that only one coroutine is modifying the instance at a time.
        sys::error_code ec;
        _bt_dht_wc.wait(_shutdown_signal, yield[ec]);
        return_or_throw_on_error(yield, _shutdown_signal, ec, _bt_dht);
        if (_bt_dht) return _bt_dht;
        auto lock = _bt_dht_wc.lock();

        auto bt_dht = make_shared<bt::MainlineDht>( _ctx.get_executor()
                                                  , _config.repo_root() / "dht");

        auto& mpl = common_udp_multiplexer();

        asio_utp::udp_multiplexer m(_ctx);

        m.bind(mpl, ec);
        if (ec) return or_throw(yield, ec, _bt_dht);

        auto cc = _shutdown_signal.connect([&] { bt_dht.reset(); });

        auto ext_ep = bt_dht->set_endpoint(move(m), yield[ec]);
        if (ec) return or_throw(yield, ec, _bt_dht);

        setup_upnp(ext_ep.port(), mpl.local_endpoint());

        _bt_dht = move(bt_dht);
        return _bt_dht;
    }

    http::response<http::string_body>
    retrieval_failure_response(const Request&);

private:
    GenericStream ssl_mitm_handshake( GenericStream&&
                                    , const Request&
                                    , asio::yield_context);

    void serve_request(GenericStream&& con, asio::yield_context yield);

    // All `fetch_*` functions below take care of keeping or dropping
    // Ouinet-specific internal HTTP headers as expected by upper layers.

    CacheEntry
    fetch_stored_in_dcache( const Request& request
                          , const request_route::Config& request_config
                          , const std::string& dht_group
                          , Cancel& cancel
                          , Yield yield);

    template<class Rq>
    Session fetch_via_self( Rq, const UserAgentMetaData&
                          , Cancel&, Yield);

    Response fetch_fresh_from_front_end(const Request&, Yield);
    Session fetch_fresh_from_origin( Request
                                   , const UserAgentMetaData&
                                   , Cancel, Yield);

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

    tcp::acceptor make_acceptor( const tcp::endpoint&
                               , const char* service) const;

    void listen_tcp( asio::yield_context
                   , tcp::acceptor
                   , function<void(GenericStream, asio::yield_context)>);

    void setup_injector(asio::yield_context);

    bool was_stopped() const {
        return _shutdown_signal.call_count() != 0;
    }

#define DEF_WAIT_FOR(WHAT) \
    void wait_for_##WHAT(Cancel& cancel, asio::yield_context yield) { \
        if (!_##WHAT##_starting) return; \
        \
        sys::error_code ec; \
        _##WHAT##_starting->wait(yield[ec]); \
        if (cancel) ec = asio::error::operation_aborted; \
        if (ec && ec != asio::error::operation_aborted) \
            LOG_ERROR("Error while waiting for " #WHAT " setup; ec=", ec.message()); \
        return or_throw(yield, ec); \
    }
    DEF_WAIT_FOR(injector)
    DEF_WAIT_FOR(cache)
#undef DEF_WAIT_FOR

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

    // Resolve host and port strings.
    TcpLookup resolve_tcp_dns( const std::string&, const std::string&
                             , Cancel&, Yield);
    TcpLookup resolve_tcp_doh( const std::string&, const std::string&
                             , const UserAgentMetaData&
                             , const doh::Endpoint&
                             , Cancel&, Yield);

    GenericStream connect_to_origin( const Request&
                                   , const UserAgentMetaData&
                                   , Cancel&, Yield);

    unique_ptr<OuiServiceImplementationClient>
    maybe_wrap_tls(unique_ptr<OuiServiceImplementationClient>);

    cache::Client* get_cache() const { return _cache.get(); }

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

        // Ensure that only one coroutine is modifying the instance at a time.
        sys::error_code ec;
        _multi_utp_server_wc.wait(_shutdown_signal, yield[ec]);
        return_or_throw_on_error(yield, _shutdown_signal, ec);
        if (_multi_utp_server) return;
        auto lock = _multi_utp_server_wc.lock();

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
                LOG_ERROR("Failed to start accepting on multi uTP service; ec=", ec.message());
                return;
            }

            while (!c) {
                sys::error_code ec;
                auto con = _multi_utp_server->accept(yield[ec]);
                if (c) return;
                if (ec == asio::error::operation_aborted) return;
                if (ec) {
                    LOG_WARN("Bep5Http: Failure to accept; ec=", ec.message());
                    async_sleep(_ctx, 200ms, c, yield);
                    continue;
                }
                TRACK_SPAWN(_ctx, ([this, con = move(con)]
                                   (asio::yield_context yield) mutable {
                    sys::error_code ec;
                    // Do not log other users' addresses unless debugging.
                    Yield y( _ctx, yield
                           , (logger.get_threshold() <= DEBUG)
                             ? "uTPAccept(" + con.remote_endpoint() + ")"
                             : "uTPAccept");
                    serve_utp_request(move(con), y[ec]);
                }));
            }
        }));
    }

private:
    // The newest protocol version number seen in a trusted exchange
    // (i.e. from an injector exchange or injector-signed cached content).
    unsigned newest_proto_seen = http_::protocol_version_current;

    // This reflects which operations have been called on the object.
    InternalState _internal_state = InternalState::Created;

    asio::io_context& _ctx;
    ClientConfig _config;
    std::unique_ptr<CACertificate> _ca_certificate;
    util::LruCache<string, string> _ssl_certificate_cache;
    std::unique_ptr<OuiServiceClient> _injector;
    std::unique_ptr<cache::Client> _cache;
    boost::optional<ConditionVariable> _injector_starting, _cache_starting;
    sys::error_code _injector_start_ec, _cache_start_ec;

    ClientFrontEnd _front_end;
    Signal<void()> _shutdown_signal;

    // For debugging
    uint64_t _next_connection_id = 0;
    ConnectionPool<Endpoint> _injector_connections;
    ConnectionPool<bool> _self_connections;  // stored value is unused
    OriginPools _origin_pools;

    asio::ssl::context ssl_ctx;
    asio::ssl::context inj_ctx;

    boost::optional<asio::ip::udp::endpoint> _local_utp_endpoint;
    boost::optional<asio_utp::udp_multiplexer> _udp_multiplexer;
    unique_ptr<util::UdpServerReachabilityAnalysis> _udp_reachability;

    shared_ptr<bt::MainlineDht> _bt_dht;
    WaitCondition _bt_dht_wc;

    unique_ptr<ouiservice::MultiUtpServer> _multi_utp_server;
    WaitCondition _multi_utp_server_wc;

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
    _YDEBUG(yield, "=== Sending back response ===");
    _YDEBUG(yield, res);

    http::async_write(con, res, yield);
}

template<class ReqBody>
static
void handle_bad_request( GenericStream& con
                       , const http::request<ReqBody>& req
                       , const string& message
                       , Yield yield)
{
    auto res = util::http_error( req, http::status::bad_request
                               , OUINET_CLIENT_SERVER_STRING
                               , "", message);
    auto yield_ = yield.tag("handle_bad_request");
    return handle_http_error(con, res, yield_);
}

//------------------------------------------------------------------------------
void
Client::State::serve_utp_request(GenericStream con, Yield yield)
{
    assert(_cache);
    if (!_cache) {
        LOG_WARN("Received uTP request, but cache is not initialized");
        return;
    }

    Cancel cancel = _shutdown_signal;

    sys::error_code ec;

    http::request<http::empty_body> req;
    beast::flat_buffer buffer;

    // We expect the first request right a way. Consecutive requests may arrive with
    // various delays.
    bool is_first_request = true;

    while (true) {
        chrono::seconds rq_read_timeout = chrono::seconds(55);

        if (is_first_request) {
            is_first_request = false;
            rq_read_timeout = chrono::seconds(5);
        }

        auto wd = watch_dog(_ctx , rq_read_timeout, [&] { con.close(); });

        http::async_read(con, buffer, req, yield[ec].tag("read"));

        if (!wd.is_running()) {
            return or_throw(yield, asio::error::timed_out);
        }

        if (ec || cancel) return;

        if (req.method() != http::verb::connect) {
            auto keep_alive = _cache->serve_local(req, con, cancel, yield[ec].tag("serve_local"));
            if (keep_alive) {
                continue;
            }
            return;
        }

        _YDEBUG(yield, "Client: Received uTP/CONNECT request");

        // Connect to the injector and tunnel the transaction through it

        if (!_bep5_client) {
            return handle_bad_request(con, req, "No known injectors", yield[ec]);
        }

        auto inj = [&] () {
            auto y = yield[ec].tag("connect_to_injector");
            return _bep5_client->connect( y
                                        , cancel
                                        , false
                                        , ouiservice::Bep5Client::injectors);
        }();

        if (cancel) ec = asio::error::operation_aborted;
        if (ec == asio::error::operation_aborted) return;
        if (ec) {
            ec = {};
            auto y = yield[ec].tag("handle_bad_request");
            return handle_bad_request(con, req, "Failed to connect to injector", y);
        }

        // Send the client an OK message indicating that the tunnel
        // has been established.
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.prepare_payload();

        // No ``res.prepare_payload()`` since no payload is allowed for CONNECT:
        // <https://tools.ietf.org/html/rfc7231#section-6.3.1>.

        {
            auto y = yield[ec].tag("write");
            http::async_write(con, res, y);
        }

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return;

        {
            auto y = yield[ec].tag("full_duplex");
            full_duplex(move(con), move(inj), y);
        }
        return;
    }
}

//------------------------------------------------------------------------------
CacheEntry
Client::State::fetch_stored_in_dcache( const Request& request
                                     , const request_route::Config& request_config
                                     , const std::string& dht_group
                                     , Cancel& cancel
                                     , Yield yield)
{
    auto c = get_cache();

    const bool cache_is_disabled
        = !c
       || !_config.is_cache_access_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheEntry>( yield
                                   , asio::error::operation_not_supported);
    }

    sys::error_code ec;

    auto key = key_from_http_req(request);
    if (!key) return or_throw<CacheEntry>(yield, asio::error::invalid_argument);
    auto s = c->load(move(*key), dht_group, request.method() == http::verb::head, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, CacheEntry{});

    s.debug();
    s.debug_prefix(yield.tag());

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
template<class Rq>
Session
Client::State::fetch_via_self( Rq request, const UserAgentMetaData& meta
                             , Cancel& cancel, Yield yield)
{
    sys::error_code ec;

    // Connect to the client proxy port.
    // TODO: Maybe refactor with `fetch_fresh_through_simple_proxy`.
    ConnectionPool<bool>::Connection con;
    if (_self_connections.empty()) {
        _YDEBUG(yield, "Connecting to self");

        // TODO: Keep lookup object or allow connecting to endpoint.
        auto epl = TcpLookup::create(_config.local_endpoint(), "dummy", "dummy");
        auto c = connect_to_host(epl, _ctx.get_executor(), cancel, yield[ec]);

        assert(!cancel || ec == asio::error::operation_aborted);

        if (ec) {
            if (ec != asio::error::operation_aborted) {
                _YERROR(yield, "Failed to connect to self; ec=", ec.message());
            }
            return or_throw<Session>(yield, ec);
        }

        con = _self_connections.wrap(std::move(c));
    } else {
        _YDEBUG(yield, "Reusing existing self connection");

        con = _self_connections.pop_front();
    }

    auto cancel_slot = cancel.connect([&] {
        con.close();
    });

    // Build the actual request to send to self.
    if (!_config.client_credentials().empty())
        request = authorize(request, _config.client_credentials());
    request.keep_alive(true);
    meta.apply_to(request);

    _YDEBUG(yield, "Sending a request to self");
    // Send request
    http::async_write(con, request, yield[ec].tag("self-request"));

    if (cancel_slot) {
        ec = asio::error::operation_aborted;
    }

    if (ec) {
        _YERROR(yield, "Failed to send request to self; ec=", ec.message());
    }

    if (ec) return or_throw<Session>(yield, ec);

    return Session::create(move(con), request.method() == http::verb::head, cancel, yield);
}

// Transforms addresses to endpoints with the given port.
template<class Addrs, class Endpoint>
class AddrsAsEndpoints {
public:
    using value_type = Endpoint;
    using addrs_iterator = typename Addrs::const_iterator;

    AddrsAsEndpoints(const Addrs& addrs, unsigned short port)
        : _addrs(addrs), _port(port)
    {}

    class const_iterator : public std::iterator<std::input_iterator_tag, value_type> {
    public:
        const_iterator(const addrs_iterator& it, unsigned short port)
            : _it(it), _port(port)
        {}

        value_type operator*() const { return {*_it, _port}; }
        const_iterator& operator++() { ++_it; return *this; }
        bool operator==(const const_iterator& other) const { return _it == other._it; }
        bool operator!=(const const_iterator& other) const { return _it != other._it; }
    private:
        addrs_iterator _it;
        unsigned short _port;
    };

    const_iterator begin() const { return {_addrs.begin(), _port}; };
    const_iterator end() const { return {_addrs.end(), _port}; };

private:
    const Addrs& _addrs;
    unsigned short _port;
};

TcpLookup
Client::State::resolve_tcp_doh( const std::string& host
                              , const std::string& port
                              , const UserAgentMetaData& meta
                              , const doh::Endpoint& ep
                              , Cancel& cancel
                              , Yield yield)
{
    using TcpEndpoint = typename TcpLookup::endpoint_type;

    boost::string_view portsv(port);
    auto portn_o = parse::number<unsigned short>(portsv);
    if (!portn_o) return or_throw<TcpLookup>(yield, asio::error::invalid_argument);

    // Build and return lookup if `host` is already a network address.
    {
        sys::error_code e;
        auto addr = asio::ip::make_address(host, e);
        if (!e) return TcpLookup::create(TcpEndpoint{move(addr), *portn_o}, host, port);
    }

    // TODO: When to disable queries for IPv4 or IPv6 addresses?
    auto rq4_o = doh::build_request_ipv4(host, ep);
    auto rq6_o = doh::build_request_ipv6(host, ep);
    if (!rq4_o || !rq6_o) return or_throw<TcpLookup>(yield, asio::error::invalid_argument);

    sys::error_code ec4, ec6;
    doh::Response rs4, rs6;

    WaitCondition wc(_ctx);

    // By passing user agent metadata as is,
    // we ensure that the DoH request is done with the same browsing mode
    // as the content request that triggered it,
    // and is announced under the same group.
    // TODO: Handle redirects.
#define SPAWN_QUERY(VER) \
    TRACK_SPAWN(_ctx, ([ \
        this, \
        rq = move(*rq##VER##_o), &meta, &ec##VER, &rs##VER, \
        &cancel, &yield, lock = wc.lock() \
    ] (asio::yield_context y_) { \
        sys::error_code ec; \
        auto y = yield.detach(y_); \
        auto s = fetch_via_self(move(rq), meta, cancel, y[ec].tag("fetch##VER")); \
        if (ec) { ec##VER = ec; return; } \
        rs##VER = http_response::slurp_response<doh::Response::body_type> \
            (s, doh::payload_size, cancel, y[ec].tag("slurp##VER")); \
        if (ec) { ec##VER = ec; return; } \
    }));
    SPAWN_QUERY(4);
    SPAWN_QUERY(6);

    wc.wait(yield.tag("wait"));

    _YDEBUG(yield, "DoH query; ip4_ec=", ec4.message(), " ip6_ec=", ec6.message());
    if (ec4 && ec6) return or_throw<TcpLookup>(yield, ec4 /* arbitrary */);

    doh::Answers answers4, answers6;
    if (!ec4) answers4 = doh::parse_response(rs4, host, ec4);
    if (!ec6) answers6 = doh::parse_response(rs6, host, ec6);

    _YDEBUG(yield, "DoH parse; ip4_ec=", ec4.message(), " ip6_ec=", ec6.message());
    if (ec4 && ec6) return or_throw<TcpLookup>(yield, ec4 /* arbitrary */);

    answers4.insert( answers4.end()
                   , std::make_move_iterator(answers6.begin())
                   , std::make_move_iterator(answers6.end()));
    AddrsAsEndpoints<doh::Answers, TcpEndpoint> eps{answers4, *portn_o};
    return TcpLookup::create(eps.begin(), eps.end(), host, port);
}

TcpLookup
Client::State::resolve_tcp_dns( const std::string& host
                              , const std::string& port
                              , Cancel& cancel
                              , Yield yield)
{
    return util::tcp_async_resolve( host, port
                                  , _ctx.get_executor()
                                  , cancel
                                  , yield);
}

GenericStream
Client::State::connect_to_origin( const Request& rq
                                , const UserAgentMetaData& meta
                                , Cancel& cancel
                                , Yield yield)
{
    std::string host, port;
    std::tie(host, port) = util::get_host_port(rq);

    sys::error_code ec;

    // Resolve using DoH if configured and not resolving the resolver's address itself.
    auto doh_ep_o = _config.origin_doh_endpoint();
    bool do_doh = doh_ep_o && !rq.target().starts_with(*doh_ep_o);
    auto lookup = do_doh
        ? resolve_tcp_doh(host, port, meta, *doh_ep_o, cancel, yield[ec].tag("resolve_doh"))
        : resolve_tcp_dns(host, port, cancel, yield[ec].tag("resolve_dns"));
    _YDEBUG( yield,  do_doh ? "DoH name resolution: " : "DNS name resolution: "
           , host, "; naddrs=", lookup.size(), " ec=", ec.message());
    return_or_throw_on_error(yield, cancel, ec, GenericStream());

    auto sock = connect_to_host(lookup, _ctx.get_executor(), cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, GenericStream());

    GenericStream stream;

    if (rq.target().starts_with("https:") || rq.target().starts_with("wss:")) {
        stream = ssl::util::client_handshake( move(sock)
                                            , ssl_ctx
                                            , host
                                            , cancel
                                            , yield[ec]);

        return_or_throw_on_error(yield, cancel, ec, GenericStream());
    }
    else {
        stream = move(sock);
    }

    return stream;
}
//------------------------------------------------------------------------------
Response Client::State::fetch_fresh_from_front_end(const Request& rq, Yield yield)
{
    Cancel cancel = _shutdown_signal;

    boost::optional<asio_utp::udp_multiplexer::endpoint_type> udp_ep;

    if (_udp_multiplexer) {
        udp_ep = _udp_multiplexer->local_endpoint();
    }

    sys::error_code ec;
    auto res = _front_end.serve( _config
                               , rq
                               , get_state()
                               , _cache.get()
                               , *_ca_certificate
                               , udp_ep
                               , _upnps
                               , _udp_reachability.get()
                               , yield[ec].tag("serve_frontend"));
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<Response>(yield, ec);

    res.set( http_::response_source_hdr  // for agent
           , http_::response_source_hdr_front_end);

    res.keep_alive(rq.keep_alive());

    return res;
}

//------------------------------------------------------------------------------
Session Client::State::fetch_fresh_from_origin( Request rq
                                              , const UserAgentMetaData& meta
                                              , Cancel cancel, Yield yield)
{
    WatchDog watch_dog(_ctx
                      , default_timeout::fetch_http()
                      , [&] { cancel(); });

    assert(!rq[http::field::host].empty());  // origin pools require host
    util::remove_ouinet_fields_ref(rq);  // avoid leaking to non-injectors

    sys::error_code ec;

    auto maybe_con = _origin_pools.get_connection(rq);
    OriginPools::Connection con;
    if (maybe_con) {
        con = std::move(*maybe_con);
    } else {
        auto stream = connect_to_origin(rq, meta, cancel, yield[ec]);

        if (cancel) {
            assert(ec == asio::error::operation_aborted);
            ec = watch_dog.is_running()
               ? ec = asio::error::operation_aborted
               : ec = asio::error::timed_out;
        }

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

    if (cancel) {
        ec = watch_dog.is_running() ? ec = asio::error::operation_aborted
                                    : ec = asio::error::timed_out;
    }
    if (ec) return or_throw<Session>(yield, ec);

    auto ret = Session::create(std::move(con), rq.method() == http::verb::head, cancel, yield[ec]);
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
    return_or_throw_on_error(yield, cancel, ec, Session());

    if (connres.result() != http::status::ok) {
        _YERROR(yield.tag("proxy_connect"), connres);
        // The use of `fetch_http<http::empty_body>` above precludes us from
        // using the response body in a normal `Session` or `Reader`,
        // so drop it altogether (TODO).
        auto rsh = util::without_framing(connres.base());
        rsh.set(http::field::content_length, 0);  // avoid blocking the agent
        util::remove_ouinet_nonerrors_ref(rsh);
        rsh.set(http_::response_source_hdr, http_::response_source_hdr_proxy);

        auto r = std::make_unique<QueueReader>(_ctx.get_executor());
        r->insert(http_response::Part(http_response::Head(std::move(rsh))));
        auto s = Session::create(std::move(r), rq.method() == http::verb::head, cancel, yield[ec]);
        return or_throw(yield, ec, std::move(s));
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

    auto session = Session::create(move(con), rq.method() == http::verb::head, cancel, yield[ec]);
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

    // Build the actual request to send to the injector (auth added below).
    if (can_inject) {
        bool keepalive = request.keep_alive();
        auto irq = util::to_injector_request(move(request));
        if (!irq) {
            _YERROR(yield, "Invalid request");
            return or_throw<Session>(yield, asio::error::invalid_argument);
        }
        request = move(*irq);
        request.keep_alive(keepalive);
    } else {
        util::remove_ouinet_fields_ref(request);  // avoid accidental injection
    }

    // Connect to the injector.
    // TODO: Maybe refactor with `fetch_via_self`.
    ConnectionPool<Endpoint>::Connection con;
    if (_injector_connections.empty()) {
        _YDEBUG(yield, "Connecting to the injector");

        auto c = _injector->connect(yield[ec].tag("connect_to_injector2"), cancel);

        assert(!cancel || ec == asio::error::operation_aborted);

        if (ec) {
            if (ec != asio::error::operation_aborted) {
                _YWARN(yield, "Failed to connect to injector; ec=", ec.message());
            }
            return or_throw<Session>(yield, ec);
        }

        assert(c.connection.has_implementation());

        con = _injector_connections.wrap(std::move(c.connection));
        *con = c.remote_endpoint;
    } else {
        _YDEBUG(yield, "Reusing existing injector connection");

        con = _injector_connections.pop_front();
    }

    auto cancel_slot = cancel.connect([&] {
        con.close();
    });

    if (auto credentials = _config.credentials_for(*con))
        request = authorize(request, *credentials);

    _YDEBUG(yield, "Sending a request to the injector");
    // Send request
    http::async_write(con, request, yield[ec].tag("inj-request"));

    if (cancel_slot) {
        ec = asio::error::operation_aborted;
    }

    if (ec) {
        _YWARN(yield, "Failed to send request to the injector; ec=", ec.message());
    }

    if (ec) return or_throw<Session>(yield, ec);

    _YDEBUG(yield, "Reading response");

    cancel_slot = {};

    // Receive response
    auto session = Session::create(move(con), request.method() == http::verb::head, cancel, yield[ec]);

    auto& hdr = session.response_header();

    if (cancel)
        ec = asio::error::operation_aborted;
    else if ( !ec
            && can_inject
            && !util::http_proto_version_check_trusted(hdr, newest_proto_seen))
        // The injector using an unacceptable protocol version is treated like
        // the Injector mechanism being disabled.
        ec = asio::error::operation_not_supported;

    _YDEBUG(yield, "End reading response; ec=", ec.message());

    if (ec) return or_throw(yield, ec, std::move(session));

    // Store keep-alive connections in connection pool

    if (can_inject) {
        maybe_add_proto_version_warning(hdr);

        hdr.set(http_::response_source_hdr, http_::response_source_hdr_injector);  // for agent
    } else {
        // Prevent others from inserting ouinet headers
        // (except a protocol error, if present and well-formed).
        util::remove_ouinet_nonerrors_ref(hdr);

        hdr.set(http_::response_source_hdr, http_::response_source_hdr_proxy);  // for agent
    }
    return session;
}

class Transaction {
public:
    Transaction(GenericStream& ua_con, const Request& rq, UserAgentMetaData meta)
        : _ua_con(ua_con)
        , _request(rq)
        , _meta(std::move(meta))
    {}

    void write_to_user_agent(Session& session, Cancel& cancel, asio::yield_context yield)
    {
        namespace err = asio::error;

        if (cancel) {
            assert(!cancel);
            LOG_ERROR(__FILE__, ":", __LINE__, " Cancel already called");
            return or_throw(yield, err::operation_aborted);
        }

        if (_ua_was_written_to) {
            return or_throw(yield, err::already_started);
        }

        sys::error_code ec;

        _ua_was_written_to = true;
        session.flush_response(_ua_con, cancel, yield[ec]);

        bool keep_alive = !ec && _request.keep_alive() && session.keep_alive();

        if (!keep_alive) {
            session.close();
            _ua_con.close();
        }

        return or_throw(yield, ec);
    }

    template<class BodyType>
    void write_to_user_agent(const http::response<BodyType>& rs, Cancel& cancel, asio::yield_context yield)
    {
        namespace err = asio::error;

        if (cancel) {
            assert(!cancel);
            LOG_ERROR(__FILE__, ":", __LINE__, " Cancel already called");
            return or_throw(yield, err::operation_aborted);
        }

        if (_ua_was_written_to) {
            return or_throw(yield, err::already_started);
        }

        sys::error_code ec;

        _ua_was_written_to = true;
        http::async_write(_ua_con, rs, yield[ec]);

        bool keep_alive = !ec && _request.keep_alive() && rs.keep_alive();

        if (!keep_alive) _ua_con.close();

        return or_throw(yield, ec);
    }

    const Request& request() const { return _request; }

    bool user_agent_was_written_to() {
        return _ua_was_written_to;
    }

    const UserAgentMetaData& meta() const { return _meta; }
private:
    /*
     * Connection to the user agent
     */
    GenericStream& _ua_con;
    const Request& _request;
    bool _ua_was_written_to = false;
    UserAgentMetaData _meta;
};

//------------------------------------------------------------------------------
class Client::ClientCacheControl {
public:
    ClientCacheControl( Client::State& client_state
                      , const request_route::Config& request_config)
        : client_state(client_state)
        , request_config(request_config)
        , cc(client_state.get_executor(), OUINET_CLIENT_SERVER_STRING)
    {
        //------------------------------------------------------------
        cc.fetch_fresh = [&] (const Request& rq, Cancel& cancel, Yield yield_) {
            auto yield = yield_.tag("injector");

            namespace err = asio::error;

            _YDEBUG(yield, "Start");

            if (!client_state._config.is_injector_access_enabled()) {
                _YDEBUG(yield, "Disabled");

                return or_throw<Session>(yield, err::operation_not_supported);
            }

            sys::error_code ec;
            auto s = client_state.fetch_fresh_through_simple_proxy( rq
                                                                  , true
                                                                  , cancel
                                                                  , yield[ec]);

            if (!ec) {
                _YDEBUG(yield, "Finish; ec=", ec.message(), " status=", s.response_header().result());
            } else {
                _YDEBUG(yield, "Finish; ec=", ec.message());
            }

            return or_throw(yield, ec, move(s));
        };

        //------------------------------------------------------------
        cc.fetch_stored = [&] (const Request& rq, const std::string& dht_group, Cancel& cancel, Yield yield_) {
            auto yield = yield_.tag("cache");

            _YDEBUG(yield, "Start");

            sys::error_code ec;
            auto r = client_state.fetch_stored_in_dcache( rq
                                                        , request_config
                                                        , dht_group
                                                        , cancel
                                                        , yield[ec]);

            _YDEBUG(yield, "Finish; ec=", ec.message(), " canceled=", bool(cancel));

            return or_throw(yield, ec, move(r));
        };

        //------------------------------------------------------------
        cc.max_cached_age(client_state._config.max_cached_age());
    }

    void front_end_job_func(Transaction& tnx, Cancel& cancel, Yield yield) {
        sys::error_code ec;
        Response res = client_state.fetch_fresh_from_front_end(tnx.request(), yield[ec]);
        if (cancel) ec = asio::error::operation_aborted;
        if (!ec) tnx.write_to_user_agent(res, cancel, yield[ec]);
        return or_throw(yield, ec);
    }

    void origin_job_func( Transaction& tnx
                        , Cancel& cancel, Yield yield) {
        if (cancel) {
            LOG_ERROR("origin_job_func received an already triggered cancel");
            return or_throw(yield, asio::error::operation_aborted);
        }

        _YDEBUG(yield, "Start");

        sys::error_code ec;
        auto session = client_state.fetch_fresh_from_origin( tnx.request(), tnx.meta()
                                                           , cancel, yield[ec]);

        _YDEBUG(yield, "Fetch; ec=", ec.message());

        return_or_throw_on_error(yield, cancel, ec);

        tnx.write_to_user_agent(session, cancel, yield[ec]);

        _YDEBUG(yield, "Flush; ec=", ec.message());

        return or_throw(yield, ec);
    }

    void proxy_job_func(Transaction& tnx, Cancel& cancel, Yield yield) {
        sys::error_code ec;

        client_state.wait_for_injector(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        _YDEBUG(yield, "Start");

        Session session;

        const auto& rq = tnx.request();

        if (rq.target().starts_with("https://")) {
            session = client_state.fetch_fresh_through_connect_proxy
                    (rq, cancel, yield[ec]);
        }
        else {
            session = client_state.fetch_fresh_through_simple_proxy
                    (rq, false, cancel, yield[ec]);
        }

        _YDEBUG(yield, "Proxy fetch; ec=", ec.message());

        return_or_throw_on_error(yield, cancel, ec);

        tnx.write_to_user_agent(session, cancel, yield[ec]);

        _YDEBUG(yield, "Flush; ec=", ec.message());

        return or_throw(yield, ec);
    }

    void injector_job_func(Transaction& tnx, Cancel& cancel, Yield yield) {
        namespace err = asio::error;

        sys::error_code ec;
        sys::error_code fresh_ec;
        sys::error_code cache_ec;

        client_state.wait_for_injector(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        client_state.wait_for_cache(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        _YDEBUG(yield, "Start");
        _YDEBUG(yield, tnx.request());

        const auto& rq   = tnx.request();
        const auto& meta = tnx.meta();

        auto session = cc.fetch(rq, meta.dht_group, fresh_ec, cache_ec, cancel, yield[ec]);

        _YDEBUG(yield, "cc.fetch; ec=", ec.message(),
                       " fresh_ec=", fresh_ec.message(),
                       " cache_ec=", cache_ec.message());

        if (ec) return or_throw(yield, ec);

        auto& rsh = session.response_header();

        _YDEBUG(yield, "Response header:");
        _YDEBUG(yield, rsh);

        assert(!fresh_ec || !cache_ec); // At least one success
        assert( fresh_ec ||  cache_ec); // One needs to fail

        auto injector_error = rsh[http_::response_error_hdr];
        if (!injector_error.empty()) {
            _YERROR(yield, "Error from injector: ", injector_error);
            tnx.write_to_user_agent(session, cancel, yield[ec]);
            return or_throw(yield, ec);
        }

        auto& ctx = client_state.get_io_context();
        auto exec = ctx.get_executor();

        using http_response::Part;

        util::AsyncQueue<boost::optional<Part>> qst(exec), qag(exec); // to storage, agent

        WaitCondition wc(ctx);

        auto cache = client_state.get_cache();

        const char* no_cache_reason = nullptr;
        bool do_cache =
            ( cache
            && rq.method() == http::verb::get  // TODO: storing HEAD response not yet supported
            && rsh[http_::response_source_hdr] != http_::response_source_hdr_local_cache
            && CacheControl::ok_to_cache( rq, rsh, client_state._config.do_cache_private()
                                        , (logger.get_threshold() <= DEBUG ? &no_cache_reason : nullptr))
            && meta.dht_group);

        if (do_cache) {
            TRACK_SPAWN(ctx, ([
                &, cache = std::move(cache),
                lock = wc.lock()
            ] (asio::yield_context yield_) {
                auto key = key_from_http_req(rq); assert(key);
                AsyncQueueReader rr(qst);
                sys::error_code ec;
                auto y_ = yield.detach(yield_);
                cache->store(*key, *meta.dht_group, rr, cancel, y_[ec]);
            }));
        } else if (no_cache_reason)
            _YDEBUG(yield, "Not ok to cache response: ", no_cache_reason);

        TRACK_SPAWN(ctx, ([
            &,
            lock = wc.lock()
        ] (asio::yield_context yield_) {
            sys::error_code ec;
            auto rr = std::make_unique<AsyncQueueReader>(qag);
            Session sag = Session::create(std::move(rr), tnx.request().method() == http::verb::head, cancel, yield_[ec]);
            if (cancel) return;
            if (ec) return;
            tnx.write_to_user_agent(sag, cancel, yield_[ec]);
        }));

        session.flush_response(cancel, yield[ec],
            [&] ( Part&& part
                , Cancel& cancel
                , asio::yield_context)
            {
                if (do_cache) qst.push_back(part);
                qag.push_back(std::move(part));
            });

        if (do_cache) qst.push_back(boost::none);
        qag.push_back(boost::none);

        wc.wait(yield);

        _YDEBUG(yield, "Finish; ec=", ec.message());

        return or_throw(yield, ec);
    }


    struct Jobs {
        enum class Type {
            front_end,
            origin,
            proxy,
            injector_or_dcache
        };

        // XXX: Currently `AsyncJob` isn't specialized for `void`, so using
        // boost::none_t as a temporary hack.
        using Retval = boost::none_t;
        using Job = AsyncJob<Retval>;

        Jobs(asio::executor exec)
            : exec(exec)
            , front_end(exec)
            , origin(exec)
            , proxy(exec)
            , injector_or_dcache(exec)
            , all({&front_end, &origin, &proxy, &injector_or_dcache})
        {}

        asio::executor exec;

        Job front_end;
        Job origin;
        Job proxy;
        Job injector_or_dcache;

        // All jobs, even those that never started.
        // Unfortunately C++14 is not letting me have array of references.
        const std::array<Job*, 4> all;

        auto running() const {
            static const auto is_running
                = [] (auto& v) { return v.is_running(); };

            return all | boost::adaptors::indirected
                       | boost::adaptors::filtered(is_running);
        }

        const char* as_string(const Job* ptr) const {
            auto type = job_to_type(ptr);
            if (!type) return "unknown";
            return as_string(*type);
        }

        static const char* as_string(Type type) {
            switch (type) {
                case Type::front_end:          return "front_end";
                case Type::origin:             return "origin";
                case Type::proxy:              return "proxy";
                case Type::injector_or_dcache: return "injector_or_dcache";
            }
            assert(0);
            return "xxx";
        };

        boost::optional<Type> job_to_type(const Job* ptr) const {
            if (ptr == &front_end)          return Type::front_end;
            if (ptr == &origin)             return Type::origin;
            if (ptr == &proxy)              return Type::proxy;
            if (ptr == &injector_or_dcache) return Type::injector_or_dcache;
            return boost::none;
        }

        Job* job_from_type(Type type) {
            switch (type) {
                case Type::front_end:          return &front_end;
                case Type::origin:             return &origin;
                case Type::proxy:              return &proxy;
                case Type::injector_or_dcache: return &injector_or_dcache;
            }
            assert(0);
            return nullptr;
        }

        size_t count_running() const {
            auto jobs = running();
            return std::distance(jobs.begin(), jobs.end());
        }

        void sleep_before_job(Type job_type, Cancel& cancel, Yield& yield) {
            size_t n = count_running();

            // 'n' includes "this" job, and we don't need to wait for that.
            assert(n > 0);
            if (n > 0) --n;

            if (job_type == Type::injector_or_dcache || job_type == Type::proxy) {
                // If origin is running, give it some time, but stop sleeping
                // if origin fetch exits early.
                if (!origin.is_running()) return;

                Cancel c(cancel);
                boost::optional<Job::Connection> jc;

                if (origin.is_running()) {
                    jc = origin.on_finish_sig([&c] { c(); });
                }

                async_sleep(exec, n * chrono::seconds(3), c, yield);
            } else if (job_type == Type::front_end) {
                // No pause for front-end jobs.
            } else {
                async_sleep(exec, n * chrono::seconds(3), cancel, yield);
            }
        }
    };

    bool is_access_enabled(Jobs::Type job_type) const {
        using Type = Jobs::Type;
        auto& cfg = client_state._config;

        switch (job_type) {
            case Type::front_end:     return true;
            case Type::origin:        return cfg.is_origin_access_enabled();
            case Type::proxy:         return cfg.is_proxy_access_enabled();
            case Type::injector_or_dcache:
                return cfg.is_injector_access_enabled()
                    || cfg.is_cache_access_enabled();
        }

        assert(0);
        return false;
    }

    // The transaction's connection is only kept open if it can still be used,
    // otherwise it is closed.
    // If an error is reported but the connection was not yet written to,
    // a response may still be sent to it
    // (please check `tnx.user_agent_was_written_to()`).
    void mixed_fetch(Transaction& tnx, Yield yield)
    {
        Cancel cancel(client_state._shutdown_signal);

        namespace err = asio::error;

        using request_route::fresh_channel;

        using Job = Jobs::Job;
        using JobCon = Job::Connection;
        using OptJobCon = boost::optional<JobCon>;

        auto exec = client_state.get_io_context().get_executor();

        Jobs jobs(exec);

        auto cancel_con = cancel.connect([&] {
            for (auto& job : jobs.running()) job.cancel();
        });

        auto start_job = [&] (Jobs::Type job_type, auto func) {
            const char* name_tag = Jobs::as_string(job_type);

            Job* job = jobs.job_from_type(job_type);

            assert(job); if (!job) return;

            if (!is_access_enabled(job_type)) {
                _YDEBUG(yield, name_tag, ": disabled");
                return;
            }

            job->start([
                &yield,
                &jobs,
                name_tag,
                func = std::move(func),
                job_type
            ] (Cancel& c, asio::yield_context y_) {
                auto y = yield.detach(y_).tag(name_tag);

                jobs.sleep_before_job(job_type, c, y);

                if (c) return or_throw(y_, err::operation_aborted, boost::none);
                sys::error_code ec;
                func(c, y[ec]);
                return or_throw(y, ec, boost::none);
            });
        };

        // TODO: When the origin is enabled and it always times out, it
        // will induce an unnecessary delay to the other routes. We need a
        // mechanism which will "realize" that other origin requests are
        // already timing out and that injector, proxy and dcache routes don't
        // need to wait for it.
        for (auto route : request_config.fresh_channels) {
            switch (route) {
                case fresh_channel::_front_end: {
                    start_job(Jobs::Type::front_end,
                            [&] (auto& c, auto y)
                            { front_end_job_func(tnx, c, y); });
                    break;
                }
                case fresh_channel::origin: {
                    start_job(Jobs::Type::origin,
                            [&] (auto& c, auto y)
                            { origin_job_func(tnx, c, y); });
                    break;
                }
                case fresh_channel::proxy: {
                    start_job(Jobs::Type::proxy,
                            [&] (auto& c, auto y)
                            { proxy_job_func(tnx, c, y); });
                    break;
                }
                case fresh_channel::injector_or_dcache: {
                    start_job(Jobs::Type::injector_or_dcache,
                            [&] (auto& c, auto y)
                            { injector_job_func(tnx, c, y); });
                    break;
                }
            }
        }

        const char* final_job = "(none)";
        boost::optional<sys::error_code> final_ec;

        auto target = tnx.request().target();
        std::string short_target = target.substr(0, 64).to_string();
        if (target.length() > 64)
            short_target.replace(short_target.end() - 3, short_target.end(), "...");

        for (size_t job_count; (job_count = jobs.count_running()) != 0;) {
            ConditionVariable cv(exec);
            std::array<OptJobCon, jobs.all.size()> cons;
            Job* which = nullptr;

            for (const auto& job : jobs.running() | boost::adaptors::indexed(0)) {
                auto i = job.index();
                auto v = &job.value();
                cons[i] = v->on_finish_sig([&cv, &which, v] {
                    if (!which) which = v;
                    cv.notify();
                });
            }

            _YDEBUG(yield, "Waiting for ", job_count, " running jobs");

            cv.wait(yield);

            if (!which) {
                _YWARN(yield, "Got result from unknown job");
                continue; // XXX
            }

            auto&& result = which->result();

            _YDEBUG( yield, "Got result; job=", jobs.as_string(which), " ec=", result.ec.message()
                   , " target=", short_target);

            if (!result.ec) {
                final_job = jobs.as_string(which);
                final_ec = sys::error_code{}; // success
                for (auto& job : jobs.running()) {
                    job.stop(yield);
                }
                break;
            } else if (!final_ec) {
                final_job = jobs.as_string(which);
                final_ec = result.ec;
            }
        }

        if (!final_ec /* not set */) {
            final_ec = err::no_protocol_option;
        }

        _YDEBUG( yield, "Done; final_job=", final_job, " final_ec=", final_ec->message()
               , " target=", short_target);

        return or_throw(yield, *final_ec);
    }

private:
    Client::State& client_state;
    const request_route::Config& request_config;
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
    // No ``res.prepare_payload()`` since no payload is allowed for CONNECT:
    // <https://tools.ietf.org/html/rfc7231#section-6.3.1>.
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
    auto meta = UserAgentMetaData::extract(rq);
    auto origin = connect_to_origin(rq, meta, cancel, yield[ec]);

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
http::response<http::string_body>
Client::State::retrieval_failure_response(const Request& req)
{
    auto res = util::http_error
        ( req, http::status::bad_gateway, OUINET_CLIENT_SERVER_STRING
        , http_::response_error_hdr_retrieval_failed
        , "Failed to retrieve the resource "
          "(after attempting all configured mechanisms)");
    maybe_add_proto_version_warning(res);
    return res;
}

//------------------------------------------------------------------------------
void Client::State::serve_request( GenericStream&& con
                                 , asio::yield_context yield_)
{
    Cancel cancel(_shutdown_signal);

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
    //
    // So enabling the Injector channel will result in caching content
    // when access to the origin is not possible.
    //
    // To also avoid getting content from the cache
    // (so that browsing looks like using a normal non-caching proxy)
    // the cache can be disabled.
    const rr::Config default_request_config
        { deque<fresh_channel>({ fresh_channel::origin
                               , fresh_channel::injector_or_dcache})};

    // This is the matching configuration for the one above,
    // but for uncacheable requests.
    const rr::Config nocache_request_config
        { deque<fresh_channel>({ fresh_channel::origin
                               , fresh_channel::proxy})};

    // The currently effective request router configuration.
    rr::Config request_config;

    Client::ClientCacheControl cache_control(*this, request_config);

    sys::error_code ec;
    beast::flat_buffer buffer;

    // Expressions to test the request against and configurations to be used.
    // TODO: Create once and reuse.
    using Match = pair<const ouinet::reqexpr::reqex, const rr::Config>;

    auto method_override_getter([](const Request& r) {return r["X-HTTP-Method-Override"];});
    auto method_getter([](const Request& r) {return r.method_string();});
    auto host_getter([](const Request& r) {return r[http::field::host];});
    auto hostname_getter([](const Request& r) {return util::split_ep(r[http::field::host]).first;});
    auto x_private_getter([](const Request& r) {return r[http_::request_private_hdr];});
    auto target_getter([](const Request& r) {return r.target();});

    auto local_rx = util::str("https?://[^:/]+\\.", _config.local_domain(), "(:[0-9]+)?/.*");

#ifdef NDEBUG // release
    const rr::Config unrequested{deque<fresh_channel>({fresh_channel::origin})};
#else // debug
    // Don't request these in debug mode as they bring a lot of noise into the log
    const rr::Config unrequested{deque<fresh_channel>()};
#endif

    static const boost::regex localhost_exact_rx{"localhost", rx_icase};

    const vector<Match> matches({
        // Please keep host-specific matches at a bare minimum
        // as they require curation and they may have undesired side-effects;
        // instead, use user agent-side mechanisms like browser settings and extensions when possible,
        // and only leave those that really break things and cannot be otherwise disabled.
        //
        // Also note that using the normal mechanisms for these may help users
        // keep their browsers up-to-date (by retrieving via the injector in case of interference),
        // and they may still not pollute the cache unless
        // the requests are explicitly marked for caching and announcement.

        // Disable cache and always go to origin for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://ident\\.me/.*")
        //     , {deque<fresh_channel>({fresh_channel::origin})} ),

        /* Requests which may be considered public but too noisy and of little value for caching
         * should be processed by something like browser extensions.
        // Google Search completion
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?google\\.com/complete/.*")
             , unrequested ),
        */

        /* To stop these requests in Firefox,
         * uncheck "Preferences / Privacy & Security / Deceptive Content and Dangerous Software Protection".
        // Safe Browsing API <https://developers.google.com/safe-browsing/>.
        // These should not be very frequent after start,
        // plus they use POST requests, so there is no risk of accidental injection.
        Match( reqexpr::from_regex(target_getter, "https://safebrowsing\\.googleapis\\.com/.*")
             , unrequested ),
        */

        /* These are used to retrieve add-ons and all kinds of minor security updates from Mozilla,
         * and they mostly happen on browser start only.
        // Disable cache and always go to origin for these mozilla sites.
        Match( reqexpr::from_regex(target_getter, "https?://content-signature\\.cdn\\.mozilla\\.net/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*services\\.mozilla\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*cdn\\.mozilla\\.net/.*")
             , unrequested ),
        */

        /* To stop these requests,
         * uncheck "Preferences / Add-ons / (gear icon) / Update Add-ons Automatically".
        // Firefox add-ons hotfix (auto-update)
        Match( reqexpr::from_regex(target_getter, "https?://services\\.addons\\.mozilla\\.org/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://versioncheck-bg\\.addons\\.mozilla\\.org/.*")
             , unrequested ),
        */

        /* To stop these requests,
         * uncheck all options from "Preferences / Privacy & Security / Firefox Data Collection and Use",
         * maybe clear `toolkit.telemetry.server` in `about:config`.
        // Firefox telemetry
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*telemetry\\.mozilla\\.net/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*telemetry\\.mozilla\\.org/.*")
             , unrequested ),
        */

        /* This should work as expected as long as Origin is enabled.
         * To stop these requests, set `network.captive-portal-service.enabled` to false in `about:config`.
        // Firefox' captive portal detection
        Match( reqexpr::from_regex(target_getter, "https?://detectportal\\.firefox\\.com/.*")
             , unrequested ),
        */

        /* To avoid these at the client, use some kind of ad blocker (like uBlock Origin).
        // Ads and tracking
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*google-analytics\\.com/.*")
             , unrequested ),

        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*googlesyndication\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*googletagservices\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*moatads\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*amazon-adsystem\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*adsafeprotected\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*ads-twitter\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*doubleclick\\.net/.*")
             , unrequested ),

        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*summerhamster\\.com/.*")
             , unrequested ),

        Match( reqexpr::from_regex(target_getter, "https?://ping.chartbeat.net/.*")
             , unrequested ),
        */

        // Handle requests to <http://localhost/> internally.
        Match( reqexpr::from_regex(host_getter, localhost_exact_rx)
             , {deque<fresh_channel>({fresh_channel::_front_end})} ),

        Match( reqexpr::from_regex(host_getter, util::str(_config.front_end_endpoint()))
             , {deque<fresh_channel>({fresh_channel::_front_end})} ),

        // Other requests to the local host should not use the network
        // to avoid leaking internal services accessed through the client.
        Match( reqexpr::from_regex(hostname_getter, util::localhost_rx)
             , {deque<fresh_channel>({fresh_channel::origin})} ),

        // Access to sites under the local TLD are always accessible
        // with good connectivity, so always use the Origin channel
        // and never cache them.
        Match( reqexpr::from_regex(target_getter, local_rx)
             , {deque<fresh_channel>({fresh_channel::origin})} ),

        // Do not use caching for requests tagged as private with Ouinet headers.
        Match( reqexpr::from_regex( x_private_getter
                                  , boost::regex(http_::request_private_true, rx_icase))
             , nocache_request_config),

        // When to try to cache or not, depending on the request method:
        //
        //   - Unsafe methods (CONNECT, DELETE, PATCH, POST, PUT): do not cache
        //   - Safe but uncacheable methods (OPTIONS, TRACE): do not cache
        //   - Safe and cacheable (GET, HEAD): cache
        //
        // Thus the only remaining method that implies caching is GET.
        Match( !reqexpr::from_regex(method_getter, "(GET|HEAD)")
             , nocache_request_config),
        // Requests declaring a method override are checked by that method.
        // This is not a standard header,
        // but for instance Firefox uses it for Safe Browsing requests,
        // which according to this standard should actually be POST requests
        // (probably in the hopes of having more chances that requests get through,
        // in spite of using HTTPS).
        Match( !reqexpr::from_regex(method_override_getter, "(|GET)")
             , nocache_request_config),

        // Disable cache and always go to proxy for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://ifconfig\\.co/.*")
        //     , {deque<fresh_channel>({fresh_channel::proxy})} ),
        // Force cache and default channels for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example\\.com/.*")
        //     , {deque<fresh_channel>()} ),
        // Force cache and particular channels for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example\\.net/.*")
        //     , {deque<fresh_channel>({fresh_channel::injector})} ),
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
                LOG_WARN("Failed to read request; ec=", ec.message());
            }
            return;
        }

        Request req(reqhp.release());

        if (!authenticate(req, con, _config.client_credentials(), yield[ec].tag("auth"))) {
            continue;
        }

        _YDEBUG(yield, "=== New request ===");
        _YDEBUG(yield, req.base());

        auto target = req.target();

        // Perform MitM for CONNECT requests (to be able to see encrypted requests)
        if (!mitm && req.method() == http::verb::connect) {
            sys::error_code ec;
            // Subsequent access to the connection will use the encrypted channel.
            con = ssl_mitm_handshake(move(con), req, yield[ec].tag("mitm_hanshake"));
            if (ec) {
                _YERROR(yield, "MitM exception; ec=", ec.message());
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
                auto host = req[http::field::host];
                if (host.empty()) {
                    req.set(http::field::host, connect_hp);
                    host = connect_hp;
                }
                req.target( string("https://")
                          + host.to_string()
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
        // Ensure that the request has a `Host:` header
        // (to ease request routing check and later operations on the head).
        if (!util::req_ensure_host(req)) {
            sys::error_code ec_;
            handle_bad_request(con, req, "Invalid or missing host in request", yield[ec_]);
            if (req.keep_alive()) continue;
            else return;
        }

        request_config = route_choose_config(req, matches, default_request_config);

        auto meta = UserAgentMetaData::extract(req);
        Transaction tnx(con, req, std::move(meta));

        if (request_config.fresh_channels.empty()) {
            _YDEBUG(yield, "Abort due to no route");
            sys::error_code ec;
            tnx.write_to_user_agent(retrieval_failure_response(req), cancel, yield[ec]);
            if (ec || cancel) return;
            continue;
        }

        cache_control.mixed_fetch(tnx, yield[ec].tag("mixed_fetch"));

        if (ec) {
            _YERROR(yield, "Error writing back response; ec=", ec.message());

            if (!tnx.user_agent_was_written_to() && !cancel && con.is_open()) {
                sys::error_code ec_;
                tnx.write_to_user_agent(retrieval_failure_response(req), cancel, yield[ec_]);
            }

            if (con.is_open() && !req.keep_alive()) con.close();
        }

        if (!con.is_open()) {
            break;
        }
    }
}

//------------------------------------------------------------------------------
void Client::State::setup_cache(asio::yield_context yield)
{
    if (_config.cache_type() != ClientConfig::CacheType::Bep5Http) return;

    LOG_DEBUG("HTTP signing public key (Ed25519): ", _config.cache_http_pub_key());

    // Remember to always set before return in case of error,
    // or the notification may not pass the right error code to listeners.
    sys::error_code ec;
    auto notify_ready = defer([&] {
        if (!_cache_starting) return;
        _cache_start_ec = ec;
        _cache_starting->notify(ec);
        _cache_starting.reset();
    });

    auto dht = bittorrent_dht(yield[ec]);
    if (ec) {
        if (ec != asio::error::operation_aborted) {
            LOG_ERROR("Failed to initialize BT DHT; ec=", ec.message());
        }
        return or_throw(yield, ec);
    }

    assert(!_shutdown_signal || ec == asio::error::operation_aborted);

    _cache = _config.cache_static_content_path().empty()
        ? cache::Client::build( dht
                              , *_config.cache_http_pub_key()
                              , _config.repo_root()/"bep5_http"
                              , _config.max_cached_age()
                              , yield[ec])
        : cache::Client::build( dht
                              , *_config.cache_http_pub_key()
                              , _config.repo_root()/"bep5_http"
                              , _config.max_cached_age()
                              , _config.cache_static_path()
                              , _config.cache_static_content_path()
                              , yield[ec]);

    if (_shutdown_signal) ec = asio::error::operation_aborted;
    if (ec) {
        if (ec != asio::error::operation_aborted) {
            LOG_ERROR("Failed to initialize cache::Client; ec="
                     , ec.message());
        }
        return or_throw(yield, ec);
    }

    idempotent_start_accepting_on_utp(yield[ec]);

    if (ec) {
        LOG_ERROR("Failed to start accepting on uTP; ec=", ec.message());
        ec = {};
    }
}

//------------------------------------------------------------------------------
tcp::acceptor Client::State::make_acceptor( const tcp::endpoint& local_endpoint
                                          , const char* service) const
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(_ctx);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) {
        throw runtime_error(util::str( "Failed to open TCP acceptor for service: ", service, "; ec="
                                     , ec.message()));
    }

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) {
        throw runtime_error(util::str( "Failed to bind TCP acceptor for service: ", service, "; ec="
                                     , ec.message()));
    }

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) {
        throw runtime_error(util::str( "Failed to 'listen' to service on TCP acceptor: ", service, "; ec="
                                     , ec.message()));
    }

    LOG_INFO("Client listening to ", service, " on TCP:", acceptor.local_endpoint());

    return acceptor;
}

//------------------------------------------------------------------------------
void Client::State::listen_tcp
        ( asio::yield_context yield
        , tcp::acceptor acceptor
        , function<void(GenericStream, asio::yield_context)> handler)
{
    auto shutdown_acceptor_slot = _shutdown_signal.connect([&acceptor] {
        acceptor.close();
    });

    WaitCondition wait_condition(_ctx);

    for(;;)
    {
        sys::error_code ec;

        tcp::socket socket(_ctx);
        acceptor.async_accept(socket, yield[ec]);

        if(ec) {
            if (ec == asio::error::operation_aborted) break;

            LOG_WARN("Accept failed on TCP:", acceptor.local_endpoint(), "; ec=", ec.message());

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
    if (_internal_state != InternalState::Created)
        return;

    InternalState next_internal_state = InternalState::Failed;
    auto set_internal_state = defer([&] {
        _internal_state = next_internal_state;
    });

    // These may throw if the endpoints are busy.
    auto proxy_acceptor = make_acceptor(_config.local_endpoint(), "browser requests");
    boost::optional<tcp::acceptor> front_end_acceptor;
    if (_config.front_end_endpoint() != tcp::endpoint())
        front_end_acceptor = make_acceptor(_config.front_end_endpoint(), "frontend");

    ssl::util::load_tls_ca_certificates(ssl_ctx, _config.tls_ca_cert_store_path());

    _ca_certificate = get_or_gen_tls_cert<CACertificate>
        ( "Your own local Ouinet client"
        , ca_cert_path(), ca_key_path(), ca_dh_path());

    if (!_config.tls_injector_cert_path().empty()) {
        if (fs::exists(fs::path(_config.tls_injector_cert_path()))) {
            LOG_DEBUG("Loading injector certificate file...");
            inj_ctx.load_verify_file(_config.tls_injector_cert_path());
            LOG_DEBUG("Loading injector certificate file: success");
        } else {
            throw runtime_error(
                    util::str("Invalid path to Injector's TLS cert file: "
                             , _config.tls_injector_cert_path()));
        }
    }

    next_internal_state = InternalState::Started;

    TRACK_SPAWN(_ctx, ([
        this,
        self = shared_from_this(),
        acceptor = move(proxy_acceptor)
    ] (asio::yield_context yield) mutable {
        if (was_stopped()) return;

        sys::error_code ec;
        listen_tcp( yield[ec]
                  , move(acceptor)
                  , [this, self]
                    (GenericStream c, asio::yield_context yield) {
                serve_request(move(c), yield);
            });
    }));

    if (front_end_acceptor) {
        TRACK_SPAWN( _ctx, ([
            this,
            self = shared_from_this(),
            acceptor = move(*front_end_acceptor)
        ] (asio::yield_context yield) mutable {
            if (was_stopped()) return;

            LOG_INFO("Serving front end on ", acceptor.local_endpoint());

            sys::error_code ec;
            listen_tcp( yield[ec]
                      , move(acceptor)
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

    TRACK_SPAWN(_ctx, ([
        this
    ] (asio::yield_context yield) {
        if (was_stopped()) return;

        sys::error_code ec;
        setup_injector(yield[ec]);

        if (ec && ec != asio::error::operation_aborted)
            LOG_ERROR("Failed to setup injector; ec=", ec.message());
    }));

    TRACK_SPAWN(_ctx, ([
        this
    ] (asio::yield_context yield) {
        if (was_stopped()) return;

        sys::error_code ec;
        setup_cache(yield[ec]);

        if (ec && ec != asio::error::operation_aborted)
            LOG_ERROR("Failed to setup cache; ec=", ec.message());
    }));
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
    // Remember to always set before return in case of error,
    // or the notification may not pass the right error code to listeners.
    sys::error_code ec;
    auto notify_ready = defer([&] {
        if (!_injector_starting) return;
        _injector_start_ec = ec;
        _injector_starting->notify(ec);
        _injector_starting.reset();
    });

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
            return or_throw(yield, ec = asio::error::invalid_argument);
        }
        */
        client = std::move(i2p_client);
    } else if (injector_ep->type == Endpoint::TcpEndpoint) {
        auto tcp_client = make_unique<ouiservice::TcpOuiServiceClient>(_ctx.get_executor(), injector_ep->endpoint_string);

        if (!tcp_client->verify_endpoint()) {
            return or_throw(yield, ec = asio::error::invalid_argument);
        }
        client = maybe_wrap_tls(move(tcp_client));
    } else if (injector_ep->type == Endpoint::UtpEndpoint) {
        asio_utp::udp_multiplexer m(_ctx);
        m.bind(common_udp_multiplexer(), ec);
        assert(!ec);

        auto utp_client = make_unique<ouiservice::UtpOuiServiceClient>
            (_ctx.get_executor(), move(m), injector_ep->endpoint_string);

        if (!utp_client->verify_remote_endpoint()) {
            return or_throw(yield, ec = asio::error::invalid_argument);
        }

        client = maybe_wrap_tls(move(utp_client));
    } else if (injector_ep->type == Endpoint::Bep5Endpoint) {
        auto dht = bittorrent_dht(yield[ec]);
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                LOG_ERROR("Failed to set up Bep5Client at setting up BT DHT; ec=", ec.message());
            }
            return or_throw(yield, ec);
        }

        boost::optional<string> bridge_swarm_name = _config.bep5_bridge_swarm_name();

        if (!bridge_swarm_name) {
            LOG_ERROR("Bridge swarm name has not been computed");
            return or_throw(yield, ec = asio::error::operation_not_supported);
        }

        _bep5_client = make_shared<ouiservice::Bep5Client>
            ( dht
            , injector_ep->endpoint_string
            , *bridge_swarm_name
            , &inj_ctx);

        client = make_unique<ouiservice::WeakOuiServiceClient>(_bep5_client);

        idempotent_start_accepting_on_utp(yield[ec]);

        if (ec) {
            LOG_ERROR("Failed to start accepting on uTP; ec=", ec.message());
            ec = {};
        }
/*
    } else if (injector_ep->type == Endpoint::LampshadeEndpoint) {
        auto lampshade_client = make_unique<ouiservice::LampshadeOuiServiceClient>(_ctx, injector_ep->endpoint_string);

        if (!lampshade_client->verify_endpoint()) {
            return or_throw(yield, ec = asio::error::invalid_argument);
        }
        client = std::move(lampshade_client);
*/
    } else if (injector_ep->type == Endpoint::Obfs2Endpoint) {
        auto obfs2_client = make_unique<ouiservice::Obfs2OuiServiceClient>(_ctx, injector_ep->endpoint_string, _config.repo_root()/"obfs2-client");

        if (!obfs2_client->verify_endpoint()) {
            return or_throw(yield, ec = asio::error::invalid_argument);
        }
        client = std::move(obfs2_client);
    } else if (injector_ep->type == Endpoint::Obfs3Endpoint) {
        auto obfs3_client = make_unique<ouiservice::Obfs3OuiServiceClient>(_ctx, injector_ep->endpoint_string, _config.repo_root()/"obfs3-client");

        if (!obfs3_client->verify_endpoint()) {
            return or_throw(yield, ec = asio::error::invalid_argument);
        }
        client = std::move(obfs3_client);
    } else if (injector_ep->type == Endpoint::Obfs4Endpoint) {
        auto obfs4_client = make_unique<ouiservice::Obfs4OuiServiceClient>(_ctx, injector_ep->endpoint_string, _config.repo_root()/"obfs4-client");

        if (!obfs4_client->verify_endpoint()) {
            return or_throw(yield, ec = asio::error::invalid_argument);
        }
        client = std::move(obfs4_client);
    }

    _injector->add(*injector_ep, std::move(client));

    _injector->start(yield[ec]);
    return or_throw(yield, ec);
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

Client::RunningState Client::get_state() const noexcept {
    return _state->get_state();
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
        LOG_ABORT(e.what());
        return 1;
    }

    ctx.run();

    LOG_INFO("Exiting gracefuly");

    return EXIT_SUCCESS;
}
#endif
