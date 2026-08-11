#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/optional/optional_io.hpp>
#include <iostream>
#include <nlohmann/json.hpp>

#include "cache/client.h"

#include "client_config.h"
#include "namespaces.h"
#include "origin_pools.h"
#include "cxx/dns.h"
#include "http_util.h"
#include "client_front_end.h"
#include "connect_to_host.h"
#include "generic_stream.h"
#include "util.h"
#include "async_sleep.h"
#include "route.h"
#include "split_string.h"
#include "request.h"
#include "peer_message.h"
#include "full_duplex_forward.h"
#include "client.h"
#include "authenticate.h"
#include "defer.h"
#include "default_timeout.h"
#include "constants.h"
#include "dispatcher.h"
#include "util/storing_reader.h"
#include "session.h"
#include "create_udp_multiplexer.h"
#include "ssl/ca_certificate.h"
#include "ssl/dummy_certificate.h"
#include "ssl/util.h"
#include "bittorrent/mainline_dht.h"
#include "bep5_swarms.h"

#include "ouiservice.h"
#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/util/create_i2p_session.h"
#include "ouiservice/tcp.h"
#include "ouiservice/utp.h"
#include "ouiservice/tls.h"
#include "ouiservice/bep5/client.h"
#include "ouiservice/multi_utp_server.h"
#include "ouiservice/ouisync/ouisync.h"

#include "parse/number.h"
#include "util/cancel.h"
#include "util/select.h"
#include "util/lru_cache.h"
#include "util/promise.h"
#include "util/spawn_for_result.h"
#include "upnp_updater.h"
#include "task.h"
#include "util/executor.h"
#include "util/debug.h"
#include "util/hash.h"

#include "task.h"
#include "logger.h"
#include "util/wait_condition.h"

using namespace std;
using namespace ouinet;

namespace bt = ouinet::bittorrent;

using tcp      = asio::ip::tcp;
using Request  = http::request<http::string_body>;
using TcpLookup = tcp::resolver::results_type;
using UdpEndpoints = std::set<asio::ip::udp::endpoint>;
using ouinet::util::AsioExecutor;
template<class V> using SysResult = std::expected<V, sys::error_code>;


static const fs::path OUINET_CA_CERT_FILE = "ssl-ca-cert.pem";
static const fs::path OUINET_CA_KEY_FILE = "ssl-ca-key.pem";
static const fs::path OUINET_CA_DH_FILE = "ssl-ca-dh.pem";
static const fs::path OUINET_ERROR_PAGE_FILE = "error-page.html";

// TODO: Put this somewhere in util/ if it turns out useful.
void throw_error(const boost::system::error_code& err)
{
    if (!err) return;
    throw boost::system::system_error(err);
}

//------------------------------------------------------------------------------
class Client::State : public enable_shared_from_this<Client::State> {
    friend class Client;

    enum class InternalState {
        Created, Failed, Started, Stopped
    };

public:
    State( asio::io_context& ctx
         , ClientConfig cfg
         , util::LogPath log_path
         , std::optional<Client::MockDhtBuilder> dht_builder)
        : _ctx(ctx)
        , _config(std::move(cfg))
        // A certificate chain with OUINET_CA + SUBJECT_CERT
        // can be around 2 KiB, so this would be around 2 MiB.
        // TODO: Fine tune if necessary.
        , _ssl_certificate_cache(1000)
        , _cache_starting{get_executor()}
        , _front_end(_config)
        , _origin_pools(OriginPools())
        , inj_ctx{asio::ssl::context::tls_client}
        , _log_path(std::move(log_path))
        , _bt_dht_builder(std::move(dht_builder))
        , _bt_dht_wc(_ctx)
        , _multi_utp_server_wc(_ctx)
        , _metrics(_config.metrics()
                    ? metrics::Client( _config.repo_root() / "metrics"
                                     , std::move(_config.metrics()->encryption_key)
                                     , _config.metrics()->delete_after_seconds)
                    : metrics::Client::noop())
        , _dns_resolver(std::make_shared<dns::Resolver>(_config.dns_config()))
    {
        LOG_INFO("Repo root: ", _config.repo_root());

        // We do *not* want to do this since
        // we will not be checking certificate names,
        // thus any certificate signed by a recognized CA
        // would be accepted if presented by an injector.
        //
        //inj_ctx.set_default_verify_paths();

        inj_ctx.set_verify_mode(asio::ssl::verify_peer);

        if (_config.metrics() && _config.metrics()->enable_on_start) {
            enable_metrics();
        }

        if (auto config = _config.ouisync_cache_config()) {
            _ouisync.emplace(_config.repo_root() / "ouisync", config->page_index_token);
        }
    }

    void start_ouinet();

    void stop_ouinet() {
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
        if (_cache_starting) _cache_starting->notify(asio::error::shut_down);

        _cache = nullptr;

        if (_upnps_ptr) _upnps_ptr->clear();

        _shutdown_signal();

        if (_injector_utp) _injector_utp.reset();
        if (_injector_i2p) _injector_i2p.reset();
        if (_bt_dht) {
            _bt_dht->stop();
            _bt_dht = nullptr;
        }

        if (_ouisync) {
            _ouisync->stop();
            _ouisync.reset();
        }

        if (_udp_multiplexer) {
            _udp_multiplexer.reset();
        }

        _origin_pools = {};
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
        if (_injector_utp) {
            if (!_injector_utp->has_result()) {
                return Client::RunningState::Starting;
            }
            else if (!_injector_utp->get_result_ref()){
                return Client::RunningState::Degraded;
            }
        }

        bool use_cache(_config.is_injecting_cache_enabled());
        bool use_cache_bep5(use_cache && _config.is_cache_enabled(CacheType::Bep5Http{}));
        if (use_cache && _cache_starting)
            return Client::RunningState::Starting;
        if (use_cache && _cache_start_ec)
            return Client::RunningState::Degraded;
        if (use_cache_bep5 && !_bt_dht->is_bootstrapped())
            return Client::RunningState::Degraded;

        return Client::RunningState::Started;
    }

    [[nodiscard]]
    std::expected<void, sys::error_code> setup_cache(Async);

    const asio_utp::udp_multiplexer& common_udp_multiplexer()
    {
        if (_udp_multiplexer) return *_udp_multiplexer;

        _udp_multiplexer
            = create_udp_multiplexer( _ctx
                                    , _config.repo_root() / "last_used_udp_port"
                                    , _config.udp_mux_port());

        return *_udp_multiplexer;
    }

    [[nodiscard]]
    std::expected<std::shared_ptr<bt::DhtBase>, sys::error_code>
    bittorrent_dht(Async yield)
    {
        if (_bt_dht) return _bt_dht;

        // Ensure that only one coroutine is modifying the instance at a time.
        if (auto r = _bt_dht_wc.wait(yield); !r) {
            return std::unexpected(r.error());
        }

        if (_bt_dht) return _bt_dht;
        auto lock = _bt_dht_wc.lock();

        std::shared_ptr<bt::DhtBase> bt_dht;

        if (_bt_dht_builder) {
            bt_dht = (*_bt_dht_builder)();
        }
        else {
            auto dht = std::make_shared<bt::MainlineDht>(
                _ctx.get_executor(),
                _metrics.mainline_dht(),
                _dns_resolver,
                _config.udp_mux_rx_limit_in_bytes(),
                _config.repo_root() / "dht",
                bt::bootstrap::Config()
                    .with_default(!_config.bt_bootstrap_no_default())
                    .with_extras(_config.bt_bootstrap_extras()),
                _log_path.tag("dht")
            );

            if (_config.bt_allow_martians()) {
                dht->set_peer_filter(bt::PeerFilter::none);
            }

            bt_dht = std::move(dht);
        }


        // Port allocation works like this:
        //
        // 1. The client tries to bind to the internal UDP port last used
        //    (a default one on first run), or a random one if it is busy.
        // 2. The BT DHT is setup to use that internal endpoint, then bootstrapped,
        //    yielding the public endpoint seen by the DHT node used too bootstrap.
        // 3. The port of that endpoint is configured as external UPnP port.
        //
        // Note that this approach still has some issues:
        //
        // - A NAT box may use different external ports depending on various factors like
        //   the remote endpoint and the presence of other devices in the LAN
        //   using the same internal port number (esp. other Ouinet clients),
        //   i.e. different bootstrap nodes may see the same or different source port numbers.
        // - If there is an extra NAT box in the middle (e.g. with CGNAT),
        //   the public port number may differ from that (or rather those) used by the "closest" NAT box,
        //   which would create a useless UPnP mapping.
        //
        // But, for the majority of cases, this may still be a reasonable bet.

        auto& mpl = common_udp_multiplexer();

        asio_utp::udp_multiplexer m(_ctx);
        m.bind(mpl);

        auto cache_control = _shutdown_signal.connect([&] { bt_dht.reset(); });

        _upnps_ptr = std::make_shared<std::map<asio::ip::udp::endpoint, unique_ptr<UPnPUpdater>>>();

        yield.spawn([
            bt_dht,
            local_ep = mpl.local_endpoint(),
            m = std::move(m),
            upnps = _upnps_ptr
        ] (auto y) mutable {
            auto ext_ep = bt_dht->add_endpoint(std::move(m)).wait(y);
            if (!ext_ep) return;

            State::setup_upnp(y.get_executor(), ext_ep->port(), local_ep, upnps);
        });

        _bt_dht = std::move(bt_dht);
        return _bt_dht;
    }

    http::response<http::string_body>
    retrieval_failure_response(const Request&);

    void enable_metrics() {
        LOG_INFO("Enabling metrics");

        _metrics.enable
            ( _ctx.get_executor()
            // Async callback executed by the metrics rust backend every time it has a record to send.
            , [ client = this
              , cancel = make_shared<Cancel>(_shutdown_signal)]
                 ( std::string_view record_name
                 , asio::const_buffer record_content
                 , asio::yield_context yield_) {
                if (*cancel) throw_error(asio::error::operation_aborted);

                Async yield(yield_, *cancel, util::LogPath("metrics"));

                try {
                    client->send_metrics_record(record_name, record_content, yield);
                } catch (std::exception& e) {
                    LOG_WARN("Failed to send metrics: ", e.what());
                    throw;
                }
            });
    }

    void disable_metrics() {
        LOG_INFO("Disabling metrics");
        _metrics.disable();
    }

private:
    std::expected<GenericStream, sys::error_code>
    ssl_mitm_handshake(GenericStream&&, const Request&, Async);

    void serve_request(GenericStream&& con, Async yield);

    [[nodiscard]]
    std::expected<ConnectionPool<Endpoint>::Connection, sys::error_code>
    get_injector_connection(InjectingCacheType, Async);

    // All `fetch_*` functions below take care of keeping or dropping
    // Ouinet-specific internal HTTP headers as expected by upper layers.

    [[nodiscard]]
    std::expected<Session, sys::error_code>
    fetch_stored_in_dcache(const CacheRetrieveRequest& request, Async);


    [[nodiscard]]
    std::expected<ClientFrontEnd::Response, sys::error_code>
    fetch_fresh_from_front_end(const Request&, Async);

    // Metrics is optional because we use this function also for sending
    // statistics which we don't want to meter.
    template<class Rq>
    std::expected<Session, sys::error_code>
    fetch_fresh_from_origin( Rq
                           , asio::ssl::context&
                           , std::optional<metrics::Request> metrics
                           , Async);

    // Metrics is optional because we use this function also for sending
    // statistics which we don't want to meter.
    template<class Rq>
    std::expected<Session, sys::error_code>
    fetch_fresh_through_connect_proxy( const Rq&
                                     , InjectingCacheType
                                     , asio::ssl::context&
                                     , std::optional<metrics::Request>
                                     , Async);

    [[nodiscard]]
    std::expected<Session, sys::error_code>
    fetch_fresh_through_simple_proxy(PublicInjectorRequest, Async);

    void
    send_metrics_record( std::string_view record_name
                       , asio::const_buffer record_content
                       , Async);

    void
    send_metrics_record( std::string_view record_name
                       , asio::const_buffer record_content
                       , MetricsServerConfig& server
                       , Async);

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

    tcp::acceptor make_acceptor( const tcp::endpoint&
                               , const char* service) const;

    asio::local::stream_protocol::acceptor make_acceptor(
            const asio::local::stream_protocol::endpoint& local_endpoint,
            const char* service) const;

    void listen_tcp( asio::yield_context
                   , tcp::acceptor
                   , function<void(GenericStream, Async)>);

    void listen_unix_socket(asio::yield_context
                          , asio::local::stream_protocol::acceptor
                          , function<void(GenericStream, Async)>);

    void setup_injectors();

    bool was_stopped() const {
        return (bool) _shutdown_signal;
    }

    inline
    std::expected<void, sys::error_code> wait_for_cache(Async yield) {
        while (true) {
            if (_cache) {
                return {};
            }

            if (!_cache_starting) {
                return std::unexpected(_cache_start_ec);
            }

            if (auto r = _cache_starting->wait(yield); !r) {
                return std::unexpected(r.error());
            }
        }
    }

    fs::path ca_cert_path() const { return _config.repo_root() / OUINET_CA_CERT_FILE; }
    fs::path ca_key_path()  const { return _config.repo_root() / OUINET_CA_KEY_FILE;  }
    fs::path ca_dh_path()   const { return _config.repo_root() / OUINET_CA_DH_FILE;   }
    fs::path error_page_path()   const { return _config.repo_root() / OUINET_ERROR_PAGE_FILE;   }

    AsioExecutor get_executor() { return _ctx.get_executor(); }

    Cancel& get_shutdown_signal() { return _shutdown_signal; }

    [[nodiscard]]
    std::expected<bool, sys::error_code>
    maybe_handle_websocket_upgrade( GenericStream&
                                  , beast::string_view connect_host_port
                                  , Request&
                                  , Async);

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code>
    connect_to_origin(const http::request_header<>& , asio::ssl::context&, Async);

    unique_ptr<OuiServiceClient>
    maybe_wrap_tls(unique_ptr<OuiServiceClient>);

    std::shared_ptr<cache::Client> get_cache() const { return _cache; }

    void serve_peer_request(InjectingCacheType, GenericStream, Async);

    [[nodiscard]]
    SysResult<Session>
    maybe_wrap_in_storing_session(const CacheRequest&, Session, Async);

    [[nodiscard]]
    SysResult<Dispatcher::Response>
    maybe_wrap_in_storing_session(Dispatcher::Response, Async);

    static void setup_upnp(
        AsioExecutor executor,
        uint16_t ext_port,
        asio::ip::udp::endpoint local_ep,
        shared_ptr<std::map<asio::ip::udp::endpoint, unique_ptr<UPnPUpdater>>> upnps
    ){
        if (!local_ep.address().is_v4()) {
            LOG_WARN("Not setting up UPnP redirection because endpoint is not ipv4");
            return;
        }

        auto &p = (*upnps)[local_ep];

        if (p) {
            LOG_WARN("UPnP redirection for ", local_ep, " is already set");
            return;
        }

        LOG_DEBUG("UPnP: Updater is starting with ",
                 "local port ", local_ep.port(), " and external port ", ext_port);
        p = make_unique<UPnPUpdater>(executor, ext_port, local_ep.port());
    }

    TaskHandle<SysResult<std::shared_ptr<I2pSession>>> get_or_create_i2p_session_task() {
        using R = SysResult<std::shared_ptr<I2pSession>>;

        if (_i2p_session_create) return *_i2p_session_create;

        _i2p_session_create = spawn_for_result(_ctx.get_executor(), _shutdown_signal, _log_path, [](Async yield) -> R {
                auto session = I2pSession::create(yield);

                if (!session) return std::unexpected(session.error());

                // Used by python test
                {
                    auto b32 = session->local_addr().to_b32();
                    LOG_DEBUG(yield, " I2P Session created, local_addr: ", b32);
                }

                return std::make_shared<I2pSession>(std::move(*session));
            });

        return *_i2p_session_create;
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    idempotent_start_accepting_on_utp(Async yield) {
        if (_multi_utp_server) return {};

        // Ensure that only one coroutine is modifying the instance at a time.
        if (auto r = _multi_utp_server_wc.wait(yield); !r) {
            return std::unexpected(r.error());
        }

        if (_multi_utp_server) return {};

        auto lock = _multi_utp_server_wc.lock();

        _multi_utp_server = make_unique<ouiservice::MultiUtpServer>(
            _ctx.get_executor()
            , UdpEndpoints{common_udp_multiplexer().local_endpoint()}, nullptr, _log_path);

        yield.tag("accept_utp").spawn([&] (Async yield) mutable {
            auto slot = yield.cancel_slot([&] () mutable {
                    _multi_utp_server = nullptr;
                });

            sys::error_code ec = _multi_utp_server->start_listen(yield);

            if (ec) {
                LOG_ERROR("Failed to start accepting on multi uTP service; ec=", ec);
                return std::unexpected(ec);
            }

            while (true) {
                auto con = _multi_utp_server->accept(yield);
                if (!con) {
                    LOG_WARN("Bep5Http: Failure to accept; ec=", con.error());
                    async_sleep(200ms, yield);
                    continue;
                }
                yield.tag("serve").spawn([this, con = std::move(*con)] (Async yield) mutable {
                    // Do not log other users' addresses unless debugging.
                    if (get_logger().get_threshold() <= DEBUG) {
                        yield = yield.tag(con.remote_endpoint());
                    }
                    serve_peer_request(CacheType::Bep5Http{}, std::move(con), yield);
                });
            }
        });

        return {};
    }

    void start_accepting_i2p(Async yield) {
        auto session = get_or_create_i2p_session_task().wait(yield);
        if (!session) return;

        if (auto tracker_addr = _config.i2p_bep3_tracker()) {
            _cache->enable_i2p(*session, *tracker_addr);
        }

        yield.spawn([this, session = std::move(*session)] (Async yield) mutable {
            while (true) {
                auto con = session->accept(yield);
                if (!con.has_value()) {
                    LOG_WARN("I2P cache: Failure to accept: ", con.error(), " is_open:", session->is_open());
                    async_sleep(200ms, yield);
                    continue;
                }
                LOG_INFO("Accepted I2P connection");
                yield.spawn([this, con = std::move(*con)] (Async yield) mutable {
                    serve_peer_request(CacheType::Bep3HTTPOverI2P{}, std::move(con), yield.tag("serve_i2p_req"));
                });
            }
        });
    }

    [[nodiscard]]
    SysResult<OuiServiceClient*> pick_injector(InjectingCacheType cache_type, Async yield) {
        auto& task = cache_type.visit(overloaded {
            [&] (CacheType::Bep5Http)        -> auto& { return _injector_utp; },
            [&] (CacheType::Bep3HTTPOverI2P) -> auto& { return _injector_i2p; }
        });

        if (!task) {
            return std::unexpected(asio::error::operation_not_supported);
        }

        auto& injector = task->wait_ref(yield);

        if (!injector) return std::unexpected(injector.error());
        return injector->get();
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

    std::optional<TaskHandle<SysResult<std::unique_ptr<OuiServiceClient>>>> _injector_utp;
    std::optional<TaskHandle<SysResult<std::unique_ptr<OuiServiceClient>>>> _injector_i2p;

    std::shared_ptr<cache::Client> _cache;
    boost::optional<ConditionVariable> _cache_starting;
    sys::error_code _cache_start_ec;

    ClientFrontEnd _front_end;
    Cancel _shutdown_signal;

    // For debugging
    uint64_t _next_connection_id = 0;
    ConnectionPool<Endpoint> _injector_connections;
    std::optional<OriginPools> _origin_pools;

    asio::ssl::context inj_ctx;

    boost::optional<asio::ip::udp::endpoint> _local_utp_endpoint;
    boost::optional<asio_utp::udp_multiplexer> _udp_multiplexer;

    util::LogPath _log_path;
    std::optional<Client::MockDhtBuilder> _bt_dht_builder;
    shared_ptr<bt::DhtBase> _bt_dht;
    WaitCondition _bt_dht_wc;

    unique_ptr<ouiservice::MultiUtpServer> _multi_utp_server;
    WaitCondition _multi_utp_server_wc;

    shared_ptr<std::map<asio::ip::udp::endpoint, unique_ptr<UPnPUpdater>>> _upnps_ptr;
    metrics::Client _metrics;

    asio::ip::tcp::endpoint _proxy_endpoint;
    // _proxy_endpoint_address is a string version of _proxy_endpoint.
    std::string _proxy_endpoint_address;
    std::string _frontend_endpoint;
    std::optional<ouisync_service::Ouisync> _ouisync;
    std::string _frontend_unix_socket_endpoint;

    // This could be created either because of cache or intent to connect to the injector
    std::optional<TaskHandle<SysResult<std::shared_ptr<I2pSession>>>> _i2p_session_create;

    shared_ptr<dns::Resolver> _dns_resolver;
    std::optional<I2pService> _i2p_service;
};

//------------------------------------------------------------------------------
template<class Token>
static
auto send_error_response( GenericStream& con
                        , bool keep_alive
                        , http::status status
                        , const string& message
                        , Token yield)
{
    auto res = util::http_error( keep_alive, status
                               , OUINET_CLIENT_SERVER_STRING
                               , "", message);

    LOG_DEBUG(yield, "=== Sending back response ===");
    LOG_DEBUG(yield, res);

    return util::http_reply(con, res, yield);
}

template<class Token>
static
auto handle_bad_request( GenericStream& con
                       , bool keep_alive
                       , const string& message
                       , Token yield)
{
    return send_error_response(con, keep_alive, http::status::bad_request, message, yield);
}

//------------------------------------------------------------------------------
void
Client::State::serve_peer_request(InjectingCacheType cache_type, GenericStream peer_con, Async yield)
{
    Cancel cancel = _shutdown_signal;
    auto cancel_slot = cancel.connect([&] { peer_con.close(); });

    // We expect the first request right a way. Consecutive requests may arrive with
    // various delays.
    bool is_first_request = true;
    beast::flat_buffer con_rbuf;  // accumulate reads across iterations here

    while (true) {
        sys::error_code ec;

        PeerRequest req;

        {
            auto rq_read_timeout = default_timeout::http_recv_simple();
            if (is_first_request) {
                is_first_request = false;
                rq_read_timeout = default_timeout::http_recv_simple_first();
            }

            auto wd = watch_dog(_ctx, rq_read_timeout, [&] { peer_con.close(); });

            auto req_r = PeerRequest::async_read(peer_con, yield.tag("read_req"));
            if (!req_r.has_value()) return;
            req = std::move(*req_r);
        }

        if (auto* cache_req = std::get_if<PeerCacheRequest>(&req)) {
            if (!_cache) {
                LOG_WARN(yield, " Received uTP request, but cache is not initialized");
                auto ec = send_error_response(peer_con, cache_req->keep_alive(), http::status::not_found
                                             , "cache not initialized", yield);
                if (ec || !cache_req->keep_alive()) return;
                continue;
            }

            if(_cache->serve_local(
                        *cache_req,
                        peer_con,
                        _metrics,
                        yield.tag("serve_local"))) {
                continue;
            }

            return;
        }

        auto connect_req = std::get_if<PeerConnectRequest>(&req);

        auto cyield = yield.tag("connect");

        if (!connect_req) {
            handle_bad_request( peer_con, false, "Invalid request", cyield.tag("invalid request"));
            return;
        }

        LOG_DEBUG(cyield, " Client: Received uTP/CONNECT request");

        // Connect to the injector and tunnel the transaction through it

        OuiServiceClient* injector = nullptr;

        if (auto r = pick_injector(cache_type, yield); !r) {
            handle_bad_request( peer_con, false, "No known injectors"
                              , cyield.tag("handle_no_injectors_error"));
            return;
        }
        else {
            injector = *r;
        }

        SysResult<GenericStream> inj_con;

        if (auto inj = dynamic_cast<ouiservice::Bep5Client*>(injector)) {
            // We're acting as a bridge, so don't connect to another bridge.
            inj_con = inj->connect( cyield.tag("connect_to_injector")
                                  , false, ouiservice::Bep5Client::injectors);

        } else {
            inj_con = injector->connect(cyield.tag("connect_to_injector"));

        }

        if (!inj_con) {
            handle_bad_request( peer_con, false, "Failed to connect to injector"
                              , cyield.tag("handle_injector_unreachable"));
            return;
        }

        // Send the client an OK message indicating that the tunnel
        // has been established.
        http::response<http::empty_body> res{http::status::ok, 11};
        res.prepare_payload();

        LOG_DEBUG(cyield, " BEGIN");

        // Remember to always set `ec` before return in case of error,
        // or the wrong error code will be reported.
        size_t fwd_bytes_c2i = 0, fwd_bytes_i2c = 0;
        auto log_result = defer([&] {
            LOG_DEBUG(cyield, " END; ec=", ec, " fwd_bytes_c2i=", fwd_bytes_c2i, " fwd_bytes_i2c=", fwd_bytes_i2c);
        });

        ec = util::http_reply(peer_con, res, cyield.tag("write_res"));
        if (ec) return;

        // Forward the rest of data in both directions.
        ec = full_duplex(
            std::move(peer_con),
            std::move(*inj_con),
            [&] (size_t byte_count) { fwd_bytes_c2i += byte_count; _metrics.bridge_transfer_c2i(byte_count); },
            [&] (size_t byte_count) { fwd_bytes_i2c += byte_count; _metrics.bridge_transfer_i2c(byte_count); },
            cyield.tag("full_duplex"));

        return;
    }
}

//------------------------------------------------------------------------------
std::expected<Session, sys::error_code>
Client::State::fetch_stored_in_dcache(const CacheRetrieveRequest& request, Async yield)
{
    using R = SysResult<Session>;

    try {
        Async timeout_yield = yield;
        auto watch_dog = ouinet::watch_dog( _ctx
                                          , default_timeout::fetch_http()
                                          , [&]{ timeout_yield.cancel(); });

        return request.visit(overloaded {
            [&] (const CachePeerRetrieveRequest& rq) -> R {
                // TODO: Should we not wait in case of the other cache type?
                if (rq.cache_type().is<CacheType::Bep5Http>()) {
                    if (auto r = wait_for_cache(timeout_yield); !r) {
                        return std::unexpected(r.error());
                    }
                }

                auto c = get_cache();

                auto s = c->load(rq, _metrics, timeout_yield.tag("load"));

                if (!s) return std::unexpected(s.error());

                auto& hdr = s->response_header();

                if (!util::http_proto_version_check_trusted(hdr, newest_proto_seen))
                    // The cached resource cannot be used, treat it like
                    // not being found.
                    return std::unexpected(asio::error::not_found);

                maybe_add_proto_version_warning(hdr);
                assert(!hdr[http_::response_source_hdr].empty());  // for agent, set by cache
                return std::move(*s);
            },
            [&] (const CacheOuisyncRetrieveRequest& rq) -> R {
                if (!_ouisync || !_ouisync->is_running()) {
                    return std::unexpected(asio::error::operation_not_supported);
                }
                return _ouisync->load(rq, yield);
            }
        });
    }
    catch (Async::Cancelled const&) {
        if (yield.is_cancelled()) throw;
        return std::unexpected(asio::error::timed_out);
    }
}

//------------------------------------------------------------------------------

std::expected<GenericStream, sys::error_code>
Client::State::connect_to_origin( const http::request_header<>& rq
                                , asio::ssl::context& tls_ctx
                                , Async yield)
{
    auto host_port = util::get_host_port(rq);

    if (!host_port) {
        return std::unexpected(asio::error::invalid_argument);
    }

    auto [host, port] = std::move(*host_port);


    auto lookup = _dns_resolver->resolve(host, port, yield);

    if (!lookup) {
        LOG_DEBUG(yield,  "DNS name resolution with protocols: [",
            dns::Resolver::protos_to_str(_config.dns_config().protocols), "]; ",
            host, "; ec=", lookup.error());
        return std::unexpected(lookup.error());
    }
    else {
        LOG_DEBUG(yield,  "DNS name resolution with protocols: [",
            dns::Resolver::protos_to_str(_config.dns_config().protocols), "]; ",
            host, "; naddrs=", lookup->size());
    }

    auto sock = connect_to_host(*lookup, yield);

    if (!sock) return std::unexpected(sock.error());

    GenericStream stream;

    if (rq.target().starts_with("https:") || rq.target().starts_with("wss:")) {
        auto sr = ssl::util::client_handshake(std::move(*sock), tls_ctx, host, yield);

        if (!sr) return std::unexpected(sr.error());
        stream = std::move(*sr);
    }
    else {
        stream = std::move(*sock);
    }

    return stream;
}
//------------------------------------------------------------------------------
std::expected<ClientFrontEnd::Response, sys::error_code>
Client::State::fetch_fresh_from_front_end(const Request& rq, Async yield)
{
    auto slot = _shutdown_signal.connect([&] { yield.cancel(); });

    boost::optional<ClientFrontEnd::UdpEndpoint> local_ep;

    if (_udp_multiplexer) {
        local_ep = _udp_multiplexer->local_endpoint();
    }

    class MetricsController : public ClientFrontEndMetricsController {
      public:
        MetricsController(Client::State* client) : client(client) {}

        void enable() override {
            client->enable_metrics();
        }

        void disable() override {
            client->disable_metrics();
        }

        bool is_enabled() const override {
            return client->_metrics.is_enabled();
        }

        std::optional<std::string> current_record_id() const override {
            return client->_metrics.current_record_id();
        }

        metrics::SetAuxResult set_aux_key_value(
                std::string_view record_id,
                std::string_view key,
                std::string_view value) override {
            return client->_metrics.set_aux_key_value(record_id, key, value);
        }

      private:
        Client::State* client;
    };

    auto metrics_controller = MetricsController(this);

    ouiservice::Bep5Client* bep5_client = nullptr;

    if (_injector_utp && _injector_utp->has_result()) {
        auto& result = _injector_utp->wait_ref(yield);
        if (result) {
            bep5_client = dynamic_cast<ouiservice::Bep5Client*>(result->get());
        }
    }

    auto res = _front_end.serve( _config
                               , rq
                               , get_state()
                               , _cache.get()
                               , bep5_client
                               , *_ca_certificate
                               , local_ep
                               , _upnps_ptr
                               , _bt_dht.get()
                               , metrics_controller
                               , _proxy_endpoint_address, _frontend_endpoint, _frontend_unix_socket_endpoint
                               , yield.tag("serve_frontend"));

    if (!res) return std::unexpected(res.error());

    res->set( http_::response_source_hdr  // for agent
            , http_::response_source_hdr_front_end);

    res->keep_alive(rq.keep_alive());

    return std::move(*res);
}

//------------------------------------------------------------------------------
template<class Rq>
std::expected<Session, sys::error_code>
Client::State::fetch_fresh_from_origin( Rq rq
                                      , asio::ssl::context& tls_ctx
                                      , std::optional<metrics::Request> metrics
                                      , Async yield)
{
    Async timeout_yield = yield;
    try {
        auto watch_dog = ouinet::watch_dog( _ctx
                                          , default_timeout::fetch_http()
                                          , [&] { timeout_yield.cancel(); });

        assert(!rq[http::field::host].empty());  // origin pools require host

        OriginPools::Connection con;

        if (!_origin_pools) {
            return std::unexpected(asio::error::operation_aborted);
        }

        auto maybe_con = _origin_pools->get_connection(rq);

        if (maybe_con) {
            con = std::move(*maybe_con);
        } else {
            auto stream = connect_to_origin(rq, tls_ctx, timeout_yield);

            if (!stream) {
                sys::error_code ec = stream.error();
                if (metrics) metrics->finish(ec);
                return std::unexpected(ec);
            }

            con = _origin_pools->wrap(rq, std::move(*stream));
        }

        // Transform request from absolute-form to origin-form
        // https://tools.ietf.org/html/rfc7230#section-5.3
        auto rq_ = util::req_form_from_absolute_to_origin(rq);

        // Send request
        {
            auto con_close = timeout_yield.cancel_slot([&] { con.close(); });
            auto r = http::async_write(con, rq_, timeout_yield.tag("write_origin_req"));

            if (!r) {
                sys::error_code ec = r.error();
                if (metrics) metrics->finish(ec);
                return std::unexpected(ec);
            }
        }

        auto ret = Session::create(
                std::move(con),
                rq.method() == http::verb::head,
                std::move(metrics),
                timeout_yield.tag("read_hdr")
            );

        if (!ret) {
            return std::unexpected(ret.error());
        }

        // Prevent others from inserting ouinet headers.
        util::remove_ouinet_fields_ref(ret->response_header());

        ret->response_header().set( http_::response_source_hdr  // for agent
                                  , http_::response_source_hdr_origin);
        return std::move(*ret);
    }
    catch (Async::Cancelled const&) {
        if (yield.is_cancelled()) throw;
        return std::unexpected(asio::error::timed_out);
    }
}

//------------------------------------------------------------------------------
template<class Rq>
std::expected<Session, sys::error_code>
Client::State::fetch_fresh_through_connect_proxy( const Rq& rq
                                                , InjectingCacheType cache_type
                                                , asio::ssl::context& tls_ctx
                                                , std::optional<metrics::Request> metrics
                                                , Async yield)
{
    // TODO: We're not re-using connections here. It's because the
    // ConnectionPool as it is right now can only work with http requests
    // and responses and thus can't be used for full-dupplex forwarding.

    return timeout(
        default_timeout::fetch_http(),
        [&](Async yield) -> std::expected<Session, sys::error_code> {
            // Parse the URL to tell HTTP/HTTPS, host, port.
            auto url = util::Url::from(rq.target());
            if (!url) {
                LOG_ERROR(yield, " Unsupported target URL");
                auto ec = asio::error::operation_not_supported;
                if (metrics) metrics->finish(ec);
                return std::unexpected(ec);
            }

            OuiServiceClient* injector = nullptr;

            if (auto r = pick_injector(cache_type, yield); !r) {
                return std::unexpected(r.error());
            }
            else {
                injector = *r;
            }

            assert(injector);

            auto inj_e = injector->connect(yield);

            if (!inj_e) {
                if (metrics) metrics->finish(inj_e.error());
                return std::unexpected(inj_e.error());
            }

            auto inj_con = std::move(*inj_e);

            // Build the actual request to send to the proxy.
            Request connreq = { http::verb::connect
                                , url->host + ":" + (url->port.empty() ? "443" : url->port)
                                , 11 /* HTTP/1.1 */};

            // HTTP/1.1 requires a ``Host:`` header in all requests:
            // <https://tools.ietf.org/html/rfc7230#section-5.4>.
            connreq.set(http::field::host, connreq.target());

            if (auto credentials = _config.injector_credentials())
                authorize(connreq, *credentials);

            // Open a tunnel to the origin
            // (to later perform the SSL handshake and send the request).
            connreq.prepare_payload();


            auto req_e = util::http_request(inj_con, connreq, yield.tag("connreq"));

            if (!req_e) {
                if (metrics) metrics->finish(req_e.error());
                return std::unexpected(req_e.error());
            }

            // Only get the head of the CONNECT response
            // (otherwise we would get stuck waiting to read
            // a body whose length we do not know
            // since a successful respone should have no content length as per RFC7231#4.3.6).
            {
                auto r = std::make_unique<http_response::Reader>(std::move(inj_con));

                auto part_e = r->async_read_part(yield.tag("read_hdr"));
                if (!part_e) {
                    if (metrics) metrics->finish(part_e.error());
                    return std::unexpected(part_e.error());
                }
                auto part = std::move(*part_e);

                assert(part && part->is_head());

                if (http::to_status_class(part->as_head()->result()) != http::status_class::successful) {
                    auto rsh = std::move(*(part->as_head()));
                    LOG_ERROR(yield.tag("proxy_connect"), " ", rsh);

                    util::remove_ouinet_nonerrors_ref(rsh);
                    rsh.set(http_::response_source_hdr, http_::response_source_hdr_proxy);

                    return Session(std::move(rsh), std::move(metrics), rq.method() == http::verb::head, std::move(r));
                }

                inj_con = r->release_stream();
            }

            std::expected<GenericStream, sys::error_code> con_e;
            if (url->scheme == "https") {
                con_e = ssl::util::client_handshake( std::move(inj_con)
                                                   , tls_ctx
                                                   , url->host
                                                   , yield);
            } else {
                con_e = std::move(inj_con);
            }

            if (!con_e) {
                if (metrics) metrics->finish(con_e.error());
                return std::unexpected(con_e.error());
            }
            auto con = std::move(*con_e);

            // TODO: move
            auto rq_ = util::req_form_from_absolute_to_origin(rq);

            auto write_e = http::async_write(con, rq_, yield.tag("write_req"));
            if (!write_e) {
                if (metrics) metrics->finish(write_e.error());
                return std::unexpected(write_e.error());
            }

            auto session_e = Session::create(
                std::move(con),
                rq.method() == http::verb::head,
                std::move(metrics),
                yield.tag("read_hdr")
            );
            if (!session_e) {
                return std::unexpected(session_e.error());
            }
            auto session = std::move(*session_e);

            // Prevent others from inserting ouinet headers.
            util::remove_ouinet_fields_ref(session.response_header());

            session.response_header().set( http_::response_source_hdr  // for agent
                                         , http_::response_source_hdr_proxy);
            return session;
        },
        yield
    );
}

//------------------------------------------------------------------------------

std::expected<
    ConnectionPool<Endpoint>::Connection,
    sys::error_code
>
Client::State::get_injector_connection(InjectingCacheType cache_type, Async yield)
{
    OuiServiceClient* injector = nullptr;

    if (auto r = pick_injector(cache_type, yield); !r) {
        return std::unexpected(r.error());
    }
    else {
        injector = *r;
    }

    assert(injector);

    if (!_injector_connections.empty()) {
        LOG_DEBUG(yield, " Reusing existing injector connection");
        return _injector_connections.pop_front();
    }

    LOG_DEBUG(yield, " Connecting to the injector");

    auto connect_e = injector->connect(yield);

    if (!connect_e) {
        LOG_WARN(yield, " Failed to connect to injector; ec=", connect_e.error());
        return std::unexpected(connect_e.error());
    }

    auto connect = std::move(*connect_e);


    auto con = _injector_connections.wrap(std::move(connect));

    return con;
}


//------------------------------------------------------------------------------
std::expected<Session, sys::error_code>
Client::State::fetch_fresh_through_simple_proxy(PublicInjectorRequest request, Async yield)
{
    auto metrics = _metrics.new_public_injector_request();

    return timeout(
        default_timeout::fetch_http(),
        [&](Async yield) -> std::expected<Session, sys::error_code> {
            auto con = get_injector_connection(request.cache_type(), yield);
            if (!con) {
                metrics.finish(con.error());
                return std::unexpected(con.error());
            }

            auto cancel_slot = yield.cancel_slot([&] { con->close(); });

            if (auto credentials = _config.injector_credentials()) {
                request.authorize(*credentials);
            }

            if (_metrics.is_enabled()) {
                if (auto druid = _metrics.current_device_id()) {
                    // Add DRUID header to the request sent to the injector
                    request.set_druid(*druid);
                }
            }

            LOG_DEBUG(yield, " Sending a request to the injector");

            // Send request
            auto write_e = request.async_write(*con, yield.tag("write_injector_req"));
            if (!write_e) {
                LOG_WARN(yield, " Failed to send request to the injector; ec=", write_e.error());
                metrics.finish(write_e.error());
                return std::unexpected(write_e.error());
            }

            LOG_DEBUG(yield, " Reading response");

            cancel_slot = {};

            // Receive response
            auto session_e = Session::create(
                std::move(*con),
                request.method() == http::verb::head,
                std::move(metrics),
                yield.tag("read_hdr")
            );

            if (!session_e) {
                LOG_WARN(yield, " Reading response failed: ", session_e.error());
                return std::unexpected(session_e.error());
            }
            auto session = std::move(*session_e);

            LOG_DEBUG(yield, " Reading response completed");

            auto& hdr = session.response_header();

            if (request.is_inject_request() &&
                !util::http_proto_version_check_trusted(hdr, newest_proto_seen))
            {
                // This is treated like the Injector mechanism being disabled.
                LOG_WARN(yield, " Injector is using an unacceptable protocol version: ", hdr);
                return std::unexpected(asio::error::operation_not_supported);
            }

            // Store keep-alive connections in connection pool

            if (request.is_inject_request()) {
                maybe_add_proto_version_warning(hdr);
                hdr.set(http_::response_source_hdr, http_::response_source_hdr_injector);  // for agent
            } else {
                // Prevent others from inserting ouinet headers
                // (except a protocol error, if present and well-formed).
                util::remove_ouinet_nonerrors_ref(hdr);
                hdr.set(http_::response_source_hdr, http_::response_source_hdr_proxy);  // for agent
            }

            return session;
        },
        yield
    );
}

void Client::State::send_metrics_record(
    std::string_view record_name,
    asio::const_buffer record_content,
    Async yield)
{
    auto metrics_conf = _config.metrics();

    if (!metrics_conf) {
        // User did not enable record sending.
        throw_error(asio::error::invalid_argument);
    }

    WaitCondition wc(yield.get_executor());

    // Send to all configured servers concurrently.
    for (auto& server_conf : metrics_conf->servers) {
        yield.spawn([&, lock = wc.lock()] (Async yield) {
            send_metrics_record(record_name, record_content, server_conf, yield);
        });
    }

    wc.wait(yield).value();
}

void Client::State::send_metrics_record(
    std::string_view record_name,
    asio::const_buffer record_content,
    MetricsServerConfig& server_conf,
    Async yield
) {
    http::request<http::buffer_body> req;

    req.version(11);
    req.method(http::verb::post);
    req.target(server_conf.url.reassemble());
    req.set(http::field::host, server_conf.url.host_and_port());
    req.set(http::field::user_agent, "Ouinet.Client");
    req.set(http::field::content_type, "application/octet-stream");
    req.set("X-Ouinet-Metrics-Record-Name", util::to_beast(record_name));

    if (server_conf.token) {
        req.set("X-Ouinet-Metrics-Server-Token", *server_conf.token);
    }

    req.body().data = const_cast<void*>(record_content.data());
    req.body().size = record_content.size();
    req.body().more = false;
    req.prepare_payload();

    auto& tls_ctx = server_conf.cacert
                  ? *server_conf.cacert
                  : _config.origin_ssl_ctx();

    // Try sending the record to the origin directly.
    auto direct_session = fetch_fresh_from_origin(req , tls_ctx , {} , yield);

    // We're only interested in the header of the response. We use this to read
    // and ignore the rest of the response so the connection can potentially be
    // reused.
    auto ignore_rest = [](Session& session, Async yield) {
        std::ignore = session.flush_response(yield, [](auto part, auto yield) {
                return std::expected<void, sys::error_code>();
            }, 60s);
    };

    if (direct_session) {
        LOG_DEBUG(yield, " Metrics direct result:", direct_session->response_header().result());

        ignore_rest(*direct_session, yield);

        if (direct_session->response_header().result() == http::status::ok) {
            return;
        } else {
            // No point in trying through the injector because we connected to
            // the origin, but it failed to process our message.
            throw_error(asio::error::invalid_argument);
        }
    }
    else {
        LOG_DEBUG(yield, " Metrics direct ec: ", direct_session.error().message());

    }

    // Sending directly failed, try sending through the injector.
    // TODO: Also try over I2P.
    auto injector_session = fetch_fresh_through_connect_proxy(req, CacheType::Bep5Http{}, tls_ctx, {}, yield);

    if (!injector_session) {
        LOG_DEBUG(yield, " Metrics injector: ec: ", injector_session.error().message());

        throw_error(injector_session.error());
    }
    else {
        LOG_DEBUG(yield, " Metrics injector: result: ", injector_session->response_header().result());
    }

    ignore_rest(*injector_session, yield);

    if (injector_session->response_header().result() != http::status::ok) {
        throw_error(asio::error::invalid_argument);
    }
}

// If preconditions are met, wrap the `response` session in another session
// which automatically stores the read HTTP parts into `client::Cache`.
SysResult<Session>
Client::State::maybe_wrap_in_storing_session(const CacheRequest& rq, Session response, Async yield) {
    auto cache = get_cache();

    if (!cache) {
        LOG_DEBUG(yield, " Not storing response because cache is not available");
        return response;
    }

    if (rq.header().method() != http::verb::get) {
        // TODO: Should we store HEAD requests?
        LOG_DEBUG(yield, " Not storing response because request is not GET");
        return response;
    }

    auto& response_hdr = response.response_header();
    auto source = response_hdr[http_::response_source_hdr];

    if (source != http_::response_source_hdr_dist_cache &&
        source != http_::response_source_hdr_injector) {
        LOG_DEBUG(yield, " Not storing response from source \"", source,"\"");
        return response;
    }

    auto injector_error = response_hdr[http_::response_error_hdr];

    if (!injector_error.empty()) {
        LOG_ERROR(yield, " Not storing response because of injector error: ", injector_error);
        return response;
    }

    const char* no_cache_reason = nullptr;

    if (!CacheControl::ok_to_cache( rq.header(), response_hdr, _config.do_cache_private()
                                  , (get_logger().get_threshold() <= DEBUG ? &no_cache_reason : nullptr))) {
        LOG_DEBUG(yield, " Not storing response because: ", no_cache_reason);
        return response;
    }

    return Session::create(
            std::make_unique<StoringReader>(rq, std::move(response), cache),
            rq.header().method() == http::verb::head,
            yield);
}

SysResult<Dispatcher::Response>
Client::State::maybe_wrap_in_storing_session(Dispatcher::Response response, Async yield) {
    using Response = Dispatcher::Response;
    using R = SysResult<Response>;

    return std::visit(overloaded {
            [&] (Response::FrontEnd r) -> R {
                return Response::FrontEnd{std::move(r.value)};
            },
            [&] (Response::Origin r) -> R {
                return Response::Origin{std::move(r.session)};
            },
            [&] (Response::DCache r) -> R {
                auto s = maybe_wrap_in_storing_session(r.request, std::move(r.session), yield);
                if (!s) return std::unexpected(s.error());
                return Response::DCache{std::move(r.request), std::move(*s)};
            },
            [&] (Response::LocalCache r) -> R {
                return Response::LocalCache{std::move(r.session)};
            },
            [&] (Response::PublicInjector r) -> R {
                auto s = maybe_wrap_in_storing_session(r.request, std::move(r.session), yield);
                if (!s) return std::unexpected(s.error());
                return Response::PublicInjector{std::move(r.request), std::move(*s)};
            },
            [&] (Response::PrivateInjector r) -> R {
                return Response::PrivateInjector{std::move(r.session)};
            },
            [&] (Response::Ouisync r) -> R {
                return Response::Ouisync{std::move(r.session)};
            }
        },
        std::move(response.value));
}

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
    return std::string(full_host.substr(dot1));
}

//------------------------------------------------------------------------------
std::expected<GenericStream, sys::error_code>
Client::State::ssl_mitm_handshake( GenericStream&& con
                                 , const Request& con_req
                                 , Async yield)
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
            = _ssl_certificate_cache.put(std::move(base_domain)
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
    if (auto r = http::async_write(con, res, yield); !r) {
        return std::unexpected(r.error());
    }

    auto ssl_sock = SslStream<GenericStream>(std::move(con), ssl_context);

    if (auto r = ssl_sock->async_handshake(asio::ssl::stream_base::server, yield); !r) {
        return std::unexpected(r.error());
    }

    return GenericStream(std::move(ssl_sock));
}

//------------------------------------------------------------------------------
std::expected<bool, sys::error_code>
Client::State::maybe_handle_websocket_upgrade( GenericStream& browser
                                             , beast::string_view connect_hp
                                             , Request& rq
                                             , Async yield)
{
    if (!boost::iequals(rq[http::field::upgrade], "websocket"))  return false;

    bool has_upgrade = false;

    for (auto s : SplitString(rq[http::field::connection], ',')) {
        if (boost::iequals(s, "Upgrade")) { has_upgrade = true; break; }
    }

    if (!has_upgrade) return false;

    if (!rq.target().starts_with("ws:") && !rq.target().starts_with("wss:")) {
        if (connect_hp.empty()) {
            handle_bad_request(browser, false, "Not a websocket server", yield);
            return true;
        }

        // Make this a "proxy" request. Among other things, this is important
        // to let the consecutive code know we want encryption.
        rq.target( util::str(
                    "wss://",
                    (rq[http::field::host].length() > 0)
                        ? rq[http::field::host]
                        : connect_hp,
                    rq.target()));
    }

    Cancel cancel(_shutdown_signal);

    // TODO: Reuse existing connections to origin and injectors.  Currently
    // this is hard because those are stored not as streams but as
    // ConnectionPool::Connection.
    auto origin = connect_to_origin(rq, _config.origin_ssl_ctx(), yield);

    if (!origin) return std::unexpected(origin.error());

    if (auto r = http::async_write(*origin, rq, yield.tag("write_req")); !r) {
        return std::unexpected(r.error());
    }

    beast::flat_buffer origin_rbuf;
    http::response<http::dynamic_body> rs;
    if (auto r = http::async_read(*origin, origin_rbuf, rs, yield.tag("read_res")); !r) {
        return std::unexpected(r.error());
    }

    if (auto r = http::async_write(browser, rs, yield.tag("write_res")); !r) {
        return std::unexpected(r.error());
    }

    if (rs.result() != http::status::switching_protocols) return true;

    sys::error_code ec;

    // First queue unused but already read data back into the origin connnection.
    if (origin_rbuf.size() > 0) origin->put_back(origin_rbuf.data(), ec);
    assert(!ec);

    // Forward the rest of data in both directions.
    ec = full_duplex(
            std::move(browser),
            std::move(*origin),
            [&] (size_t) {},
            [&] (size_t) {},
            yield.tag("full_duplex"));

    if (ec) return std::unexpected(ec);

    return true;
}

static
string file_to_string(std::string fname)
{
    using std::ios;

    std::fstream file_stream;
    ostringstream out_ss;

    if (fname.empty()) {
        return out_ss.str();
    }

    if (ouinet::fs::exists(fname)) {
        file_stream.open(fname, ios::in);
    } else {
        // File doesn't exist return empty string
        return out_ss.str();
    }

    if (!file_stream.is_open()) {
        std::cerr << "Failed to open file " << fname  << "\n";
    } else {
        std::copy( istreambuf_iterator<char>(file_stream)
                    , istreambuf_iterator<char>()
                    , ostreambuf_iterator<char>(out_ss));
    }
    return out_ss.str();
}

//------------------------------------------------------------------------------
http::response<http::string_body>
Client::State::retrieval_failure_response(const Request& req)
{
    http::response<http::string_body> res;
    std::string content = file_to_string(error_page_path().string());
    if (content.empty()) {
        res = util::http_error
            ( req.keep_alive(), http::status::bad_gateway, OUINET_CLIENT_SERVER_STRING
              , http_::response_error_hdr_retrieval_failed
              , "Failed to retrieve the resource "
              "(after attempting all configured mechanisms)");
    }
    else {
        res = util::http_error_html
            ( req, http::status::bad_gateway, OUINET_CLIENT_SERVER_STRING
              , http_::response_error_hdr_retrieval_failed
              , content);
    }
    maybe_add_proto_version_warning(res);
    return res;
}

//------------------------------------------------------------------------------
void Client::State::serve_request(GenericStream&& con, Async yield_)
{
    Cancel cancel(_shutdown_signal);

    auto close_con_slot = _shutdown_signal.connect([&con] {
        con.close();
    });

    struct Routes : Dispatcher::Routes {
        SysResult<ClientFrontEnd::Response>
        front_end(const Request& rq, Async yield) override {
            return client_state.fetch_fresh_from_front_end(rq, yield);
        }

        SysResult<Session>
        origin(const Request& rq_, Async yield) override {
            auto rq = rq_;
            // Avoid leaking to non-injectors
            util::remove_ouinet_fields_ref(rq);

            auto metrics = client_state._metrics.new_origin_request();

            return client_state.fetch_fresh_from_origin( rq
                                                       , client_state._config.origin_ssl_ctx()
                                                       , std::move(metrics)
                                                       , yield);
        }

        SysResult<Session>
        public_injector(const CacheInjectRequest& rq, Async yield) override {
            return client_state.fetch_fresh_through_simple_proxy(rq, yield);
        }

        SysResult<Session>
        private_injector(InjectingCacheType cache_type, const Request& rq_, Async yield) override {
            auto rq = rq_;

            SysResult<Session> session;

            if (rq.target().starts_with("https://")) {
                auto metrics = client_state._metrics.new_private_injector_request();

                util::remove_ouinet_fields_ref(rq);

                session = client_state.fetch_fresh_through_connect_proxy(
                    rq,
                    cache_type,
                    client_state._config.origin_ssl_ctx(),
                    std::move(metrics),
                    yield.tag("connect")
                );
            }
            else {
                auto insecure_rq = InsecureRequest::from(cache_type, std::move(rq));

                if (!insecure_rq) {
                    return std::unexpected(asio::error::invalid_argument);
                }

                session = client_state.fetch_fresh_through_simple_proxy(
                    std::move(*insecure_rq),
                    yield.tag("simple")
                );
            }

            LOG_DEBUG(yield, " Proxy fetch; ec=", session ? sys::error_code() : session.error());

            return session;
        }

        SysResult<Session>
        distributes_cache(const CacheRetrieveRequest& rq, Async yield) override {
            return client_state.fetch_stored_in_dcache(rq, yield);
        }

        boost::posix_time::time_duration max_cached_age() override {
            return client_state._config.max_cached_age();
        }

        bool is_injector_starting() override {
            return client_state._injector_utp && !client_state._injector_utp->has_result();
        }

        Routes(State& client_state) : client_state(client_state) {}
        State& client_state;
    };

    Routes routes(*this);
    Dispatcher dispatcher(yield_.get_executor(), routes);

    auto connection_id = _next_connection_id++;
    auto connection_idstr = util::str('C', connection_id);

    // Is MitM active?
    bool mitm = false;

    // Saved host/port from CONNECT request.
    string connect_hp;

    // Process the different requests that may come over the same connection.
    beast::flat_buffer con_rbuf;  // accumulate reads across iterations here

    uint64_t next_request_id = 0;

    for (;;) {  // continue for next request; break for no more requests
        // Read the (clear-text) HTTP request
        // (without a size limit, in case we are uploading a big file).
        // Based on <https://stackoverflow.com/a/50359998>.
        http::request_parser<Request::body_type> reqhp;
        if (_config.max_request_body_size() == 0) {
            reqhp.body_limit((std::numeric_limits<std::uint64_t>::max)());
        } else {
            reqhp.body_limit(_config.max_request_body_size());
        }
        reqhp.header_limit(16*1024);

        // No timeout either, a keep-alive connection to the user agent
        // will remain open and waiting for new requests
        // until the later desires to close it.
        Async yield = yield_.tag(util::str("R", next_request_id++));

        auto read_r = http::async_read(con, con_rbuf, reqhp, yield.tag("read_req"));

        if (!read_r) {
            auto ec = read_r.error();

            if ( ec != http::error::end_of_stream
              && ec != asio::ssl::error::stream_truncated) {
                LOG_WARN(yield.log_path(), " Failed to read request; ec=", ec);
            }

            break;
        }

        Request req(reqhp.release());
        auto req_done = defer([&yield] { LOG_DEBUG(yield, " Done"); });

        {
            auto& fields = _config.add_request_fields();

            if (!fields.empty()) {
                LOG_WARN(yield, " Adding request fields:");
            }

            for (const auto& [key, value] : fields) {
                LOG_WARN(yield, "   ", key, ": ", value);
                req.set(key, value);
            }
        }

#if defined(__MACH__)
        // It is not possible to inject headers into every request made
        // by WebKit on iOS, but we can modifiy the User Agent.
        // Check if X-Ouinet-Private string is included in the User Agent.
        auto ua = req[http::field::user_agent];
        if (!ua.empty()) {
            size_t index = ua.find("X-Ouinet-Private");
            if (index != std::string::npos ){
                req.set(http::field::user_agent, ua.substr(0, index));
                req.set(http_::request_private_hdr, http_::request_private_true);
            }
        }
#endif

        // "authenticate" function strips proxy_authorization headers.
        // Authentication is needed only for the outer HTTP SSL CONNECT request,
        // not the inner HTTP GET inside the SSL.
        if (!mitm) {
            auto auth = authenticate(req, con, _config.client_credentials(), yield.tag("auth"));
            if (!auth) {
                LOG_WARN(yield, " Request authentication failed, discarding; ec=", auth.error());
                continue;
            }
        }

        LOG_DEBUG(yield, " === New request ===");
        LOG_DEBUG(yield, " ", req.base());

        auto target = req.target();

        // Perform MitM for CONNECT requests (to be able to see encrypted requests)
        if (!mitm && req.method() == http::verb::connect) {
            // Subsequent access to the connection will use the encrypted channel.
            if (auto r = ssl_mitm_handshake(std::move(con), req, yield.tag("mitm_handshake")); !r) {
                LOG_ERROR(yield, " MitM exception; ec=", r.error());
                break;
            }
            else {
                con = std::move(*r);
            }
            mitm = true;
            // Save CONNECT target (minus standard HTTPS port ``:443`` if present)
            // in case of subsequent HTTP/1.0 requests with no ``Host:`` header.
            auto port_pos = max( target.length() - 4 /* strlen(":443") */
                               , string::npos);
            connect_hp = string(target
                // Do not to hit ``:443`` inside of an IPv6 address.
                .substr(0, target.rfind(":443", port_pos)));
            // Go for requests in the encrypted channel.
            continue;
        }

        if (auto r = maybe_handle_websocket_upgrade( con
                                                   , connect_hp
                                                   , req
                                                   , yield.tag("websocket")); !r || *r) {
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
                req.target(util::str("https://", host, target));
                target = req.target();
            } else {
                // TODO: Maybe later we want to support front-end and API calls
                // as plain HTTP requests (as if we were a plain HTTP server)
                // but for the moment we only accept proxy requests.
                auto r = handle_bad_request(con, req.keep_alive(), "Not a proxy request", yield);
                if (r && req.keep_alive()) continue;
                else break;
            }
        }

        if (auto& token = _config.proxy_access_token()) {
            std::string_view header_key = "X-Ouinet-Proxy-Token";
            if (*token != req[header_key]) {
                auto message = "The request is missing a valid "
                    + std::string(header_key)
                    + " HTTP header\n";
                auto r = send_error_response(con, req.keep_alive(), http::status::unauthorized, message, yield);
                if (r && req.keep_alive()) continue;
                else break;
            }
        }

        // Ensure that the request has a `Host:` header
        // (to ease request routing check and later operations on the head).
        if (!util::req_ensure_host(req)) {
            auto r = handle_bad_request(con, req.keep_alive(), "Invalid or missing host in request", yield);
            if (r && req.keep_alive()) continue;
            else break;
        }

        auto route = Route::choose(req, _config);

        LOG_DEBUG(yield, " Chosen route: ", debug(route));

        if (!route) {
            LOG_WARN(yield, " Failed to choose route for request");
            auto rs = retrieval_failure_response(req);
            auto r = http::async_write(con, rs, yield);
            if (!r || !req.keep_alive() || !rs.keep_alive()) break;
            continue;
        }

        auto response = dispatcher.dispatch(req, *route, yield);

        if (!response) {
            LOG_DEBUG(yield, " Failed to receive a response: ", response.error());
            auto rs = retrieval_failure_response(req);
            auto r = http::async_write(con, rs, yield);
            if (!r || !req.keep_alive() || !rs.keep_alive()) break;
            continue;
        }

        response = maybe_wrap_in_storing_session(std::move(*response), yield);

        if (!response) {
            LOG_DEBUG(yield, " Failed wrap response in StoringSession: ", response.error());
            auto rs = retrieval_failure_response(req);
            auto r = http::async_write(con, rs, yield);
            if (!r || !req.keep_alive() || !rs.keep_alive()) break;
            continue;
        }

        LOG_DEBUG(yield, " Response: ", response->header());

        if (auto r = response->write(con, yield); !r) {
            LOG_DEBUG(yield, " Failed to write response to UA: ", response.error());
            auto rs = retrieval_failure_response(req);
            auto wr = http::async_write(con, rs, yield);
            if (!wr || !req.keep_alive() || !rs.keep_alive()) break;
            continue;
        }

        if (!req.keep_alive() || !response->keep_alive()) {
            break;
        }
    }

    if (con.is_open()) con.close();

    LOG_DEBUG(yield_, " Done");
}

//------------------------------------------------------------------------------
std::expected<void, sys::error_code>
Client::State::setup_cache(Async yield)
{
    // Remember to always set before return in case of error,
    // or the notification may not pass the right error code to listeners.
    sys::error_code ec;
    auto do_notify_ready = [&] {
        if (!_cache_starting) return;
        _cache_start_ec = ec;
        _cache_starting->notify(ec);
        _cache_starting.reset();
    };

    auto notify_ready = defer([&] {
        do_notify_ready();
    });

    if (!_config.is_cache_enabled(CacheType::Bep5Http{})
        && !_config.is_cache_enabled(CacheType::Bep3HTTPOverI2P{}))
    {
        return {};
    }

    LOG_DEBUG("HTTP signing public key (Ed25519): ", _config.cache_http_pub_key());

    if (auto r = _config.cache_static_content_path().empty()
        ? cache::Client::build( UdpEndpoints{common_udp_multiplexer().local_endpoint()}
                              , *_config.cache_http_pub_key()
                                , _config.repo_root()/"bep5_http" //TODO gives this a more inclusive name covering bothe bep5 and bep3 caches
                              , _config.max_cached_age()
                              , yield)
        : cache::Client::build( UdpEndpoints{common_udp_multiplexer().local_endpoint()}
                              , *_config.cache_http_pub_key()
                              , _config.repo_root()/"bep5_http"
                              , _config.max_cached_age()
                              , _config.cache_static_path()
                              , _config.cache_static_content_path()
                              , yield)) {
        _cache = std::move(*r);
    }
    else {
        LOG_ERROR(yield, " Failed to initialize cache::Client");
        return std::unexpected(r.error());
    }

    if (auto r = idempotent_start_accepting_on_utp(yield); !r) {
        LOG_ERROR(yield, " Failed to start accepting on uTP for cache::Client");
        return std::unexpected(r.error());
    }

    // Subsequent calls below will not alter cache start result,
    // but they will still report and error code to the caller.
    do_notify_ready();

    if (_config.is_cache_enabled(CacheType::Bep5Http{})) {
        auto dht = bittorrent_dht(yield);
        if (!dht) {
            LOG_ERROR(yield, " Failed to initialize BT DHT for cache::Client");
            return std::unexpected(dht.error());
        }

        if (!_cache->enable_dht(*dht, _config.max_simultaneous_announcements())) {
            LOG_ERROR(yield, " Failed to enable BT DHT in cache::Client");
            return std::unexpected(asio::error::invalid_argument);
        }
    }

    if (_config.is_cache_enabled(CacheType::Bep3HTTPOverI2P{})) {
        start_accepting_i2p(yield);
    }

    return {};
}

#ifdef _WIN32
namespace boost::asio {
    typedef detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE> socket_base__exclusive_address_use;
}
#endif

//------------------------------------------------------------------------------
tcp::acceptor Client::State::make_acceptor( const tcp::endpoint& local_endpoint
                                          , const char* service) const
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(_ctx);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) {
        throw runtime_error(util::str("Failed to open TCP acceptor for service: ", service, "; ec=", ec));
    }

    // Windows and Unix have a completely different understanding of what SO_REUSEADDR means.
    // On Unix it means that you can close a bound socket and then open a new one and bind it to the same port right away.
    // On Windows it means that several unrelated programs from different users can bind to the same port.
    // According to MSDN "Once the second socket has successfully bound, the behavior for all sockets bound to that port is indeterminate".
    // https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse
    // Let us not do that on Windows. Closest Unix equivalent of SO_REUSEADDR on Windows is SO_DONTLINGER.
    // Also set SO_EXCLUSIVEADDRUSE, to prevent other sockets from binding to the same port.
#ifdef _WIN32
    acceptor.set_option(asio::socket_base::linger(false, 0));
    acceptor.set_option(asio::socket_base__exclusive_address_use(true));
#else
    acceptor.set_option(asio::socket_base::reuse_address(true));
#endif

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) {
        throw runtime_error(
            util::str("Failed to bind TCP acceptor on port "
                     , local_endpoint.port()
                     , " for service: "
                     , service, "; ec=", ec));
    }

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw runtime_error(util::str("Failed to 'listen' to service on TCP acceptor: ", service, "; ec=", ec));
    }

    LOG_INFO("Client listening to ", service, " on TCP:", acceptor.local_endpoint());

    return acceptor;
}

//------------------------------------------------------------------------------
asio::local::stream_protocol::acceptor Client::State::make_acceptor(
    const asio::local::stream_protocol::endpoint& local_endpoint,
    const char* service) const
{
    sys::error_code ec;

    // Open the acceptor
    asio::local::stream_protocol::acceptor acceptor(_ctx);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) {
        throw runtime_error(util::str("Failed to open Unix Socket acceptor for service: ", service, "; ec=", ec));
    }

    acceptor.set_option(asio::socket_base::reuse_address(false));

    const fs::path socket_path(local_endpoint.path());
    const fs::file_status socket_status = fs::status(socket_path);
    if (fs::exists(socket_status)) {
        // On windows boost::filesystem::is_socket does not detect af_unix sockets.
        // std::filesystem::is_socket is also oblivious to af_unix sockets on windows.
        // boost::filesystem detects it as reparse file.
        if (fs::is_socket(socket_status) || fs::is_reparse_file(socket_status)) {
            fs::remove(socket_path);
        } else {
            throw runtime_error(util::str(
                "File already exists where frontend unix socket would be created. "
                "Refusing to auto delete it: ", local_endpoint.path()));
        }
    }

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) {
        throw runtime_error(util::str("Failed to bind Unix Socket acceptor for service: ", service, "; ec=", ec));
    }

    fs::permissions(socket_path, fs::perms::owner_all, ec);
    if (ec) {
        throw runtime_error(util::str("Failed to chmod 700 the Unix Socket: ", service, "; ec=", ec));
    }

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw runtime_error(util::str("Failed to 'listen' to service on Unix Socket acceptor: ", service, "; ec=", ec));
    }

    LOG_INFO("Client listening to ", service, " on Unix Socket:", acceptor.local_endpoint());

    return acceptor;
}

//------------------------------------------------------------------------------
void Client::State::listen_tcp
        ( asio::yield_context yield
        , tcp::acceptor acceptor
        , function<void(GenericStream, Async)> handler)
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

        if (ec) {
            if (ec == asio::error::operation_aborted) break;

            LOG_WARN(_log_path, " Accept failed on TCP:", acceptor.local_endpoint(), "; ec=", ec);

            if (!async_sleep(chrono::seconds(1), _shutdown_signal, yield)) {
                break;
            }
        } else {
            static const auto tcp_shutter = [](tcp::socket& s) {
                sys::error_code ec; // Don't throw
                s.shutdown(tcp::socket::shutdown_both, ec);
                s.close(ec);
            };

            GenericStream connection(std::move(socket) , std::move(tcp_shutter));

            spawn_detached(_ctx.get_executor(), _shutdown_signal, _log_path, [
                self = shared_from_this(),
                c = std::move(connection),
                handler,
                lock = wait_condition.lock()
            ] (Async yield) mutable {
                if (self->was_stopped()) return;
                handler(std::move(c), yield);
            });
        }
    }

    wait_condition.wait(yield);
}

//------------------------------------------------------------------------------
void Client::State::listen_unix_socket
        ( asio::yield_context yield
        , asio::local::stream_protocol::acceptor acceptor
        , function<void(GenericStream, Async)> handler)
{
    auto shutdown_acceptor_slot = _shutdown_signal.connect([&acceptor] {
        const auto endpoint_path = fs::path(acceptor.local_endpoint().path());
        acceptor.close();
        if (fs::exists(endpoint_path)) {
            fs::remove(endpoint_path);
        }
    });

    WaitCondition wait_condition(_ctx);

    for(;;)
    {
        sys::error_code ec;

        asio::local::stream_protocol::socket socket(_ctx);
        acceptor.async_accept(socket, yield[ec]);

        if (ec) {
            if (ec == asio::error::operation_aborted) break;

            LOG_WARN("Accept failed on Unix Socket:", acceptor.local_endpoint(), "; ec=", ec);

            if (!async_sleep(chrono::seconds(1), _shutdown_signal, yield)) {
                break;
            }
        } else {
            static const auto unix_socket_shutter = [](asio::local::stream_protocol::socket& s) {
                sys::error_code ec; // Don't throw
                s.shutdown(asio::local::stream_protocol::socket::shutdown_both, ec);
                s.close(ec);
            };

            GenericStream connection(std::move(socket) , std::move(unix_socket_shutter));

            spawn_detached(_ctx.get_executor(), _shutdown_signal, _log_path.tag("unix_socket"), [
                self = shared_from_this(),
                c = std::move(connection),
                handler,
                lock = wait_condition.lock()
            ] (Async yield) mutable {
                if (self->was_stopped()) return;
                handler(std::move(c), yield);
            });
        }
    }

    wait_condition.wait(yield);
}

//------------------------------------------------------------------------------
void Client::State::start_ouinet()
{
    if (_internal_state != InternalState::Created)
        return;

    InternalState next_internal_state = InternalState::Failed;
    auto set_internal_state = defer([&] {
        _internal_state = next_internal_state;
    });

    // These may throw if the endpoints are busy.
    auto proxy_acceptor = make_acceptor(_config.local_endpoint(), "browser requests");
    _proxy_endpoint = proxy_acceptor.local_endpoint();
    _proxy_endpoint_address = std::string(_proxy_endpoint.address().to_string()) + ":" + to_string(_proxy_endpoint.port());

    boost::optional<tcp::acceptor> front_end_acceptor;
    if (_config.front_end_endpoint() != tcp::endpoint())
    {
        front_end_acceptor = make_acceptor(_config.front_end_endpoint(), "frontend");
        _frontend_endpoint = string(front_end_acceptor->local_endpoint().address().to_string()) + ":"
                           + to_string(front_end_acceptor->local_endpoint().port());
    }

    boost::optional<asio::local::stream_protocol::acceptor> front_end_unix_socket_acceptor;
    if (_config.front_end_unix_socket_endpoint() != asio::local::stream_protocol::endpoint()) {
        LOG_DEBUG("front_end_unix_socket endpoint: ", _config.front_end_unix_socket_endpoint());
        front_end_unix_socket_acceptor = make_acceptor(_config.front_end_unix_socket_endpoint(), "frontend_unix_socket");
        _frontend_unix_socket_endpoint = front_end_unix_socket_acceptor->local_endpoint().path();
    }

    {
        const nlohmann::json endpoints_json = {
            {"proxy_endpoint", _proxy_endpoint_address},
            {"frontend_tcp_endpoint", _frontend_endpoint},
            {"frontend_unix_socket_endpoint", _frontend_unix_socket_endpoint},
        };
        const fs::path endpoints_json_file { _config.repo_root() / "endpoints.json" };
        boost::nowide::ofstream(endpoints_json_file) << endpoints_json;
    }

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

    if (_ouisync) {
        spawn_detached(_ctx.get_executor(), _shutdown_signal, _log_path, [
            self = shared_from_this()
        ] (Async yield) mutable {
            sys::error_code ec = self->_ouisync->start(yield);

            if (!ec) {
                LOG_INFO(yield, " Ouisync started");
            }
            else {
                LOG_ERROR(yield, " Failed to start Ouisync: ", ec.message());
            }
        });
    }

    task::spawn_detached(_ctx, [
        this,
        self = shared_from_this(),
        acceptor = std::move(proxy_acceptor)
    ] (asio::yield_context yield) mutable {
        if (was_stopped()) return;

        sys::error_code ec;
        listen_tcp( yield[ec]
                  , std::move(acceptor)
                  , [this, self]
                    (GenericStream c, Async yield) {
                auto connection_id = _next_connection_id++;

                auto y = yield.tag(util::str('C', connection_id));

                LOG_DEBUG(y, " Accepted connection from UA");

                serve_request(std::move(c), y);
            });
    });

    if (front_end_acceptor) {
        task::spawn_detached( _ctx, [
            this,
            self = shared_from_this(),
            acceptor = std::move(*front_end_acceptor)
        ] (asio::yield_context yield) mutable {
            if (was_stopped()) return;

            LOG_INFO("Serving front end on ", acceptor.local_endpoint());

            sys::error_code ec;
            listen_tcp( yield[ec]
                      , std::move(acceptor)
                      , [this, self]
                        (GenericStream c, Async yield_) {
                  Async yield = yield_.tag("frontend");
                  beast::flat_buffer c_rbuf;
                  Request rq;
                  if (auto r = http::async_read(c, c_rbuf, rq, yield.tag("read_req")); !r) {
                      return;
                  }

                  auto rs = fetch_fresh_from_front_end(rq, yield.tag("get_res"));

                  if (!rs) return;

                  http::async_write(c, *rs, yield.tag("write_res"));
            });
        });
    }

    if (front_end_unix_socket_acceptor) {
        task::spawn_detached( _ctx, [
            this,
            self = shared_from_this(),
            acceptor = std::move(*front_end_unix_socket_acceptor)
        ] (asio::yield_context yield) mutable {
            if (was_stopped()) return;

            LOG_INFO("Serving front end on ", acceptor.local_endpoint());

            sys::error_code ec;
            listen_unix_socket( yield[ec]
                      , std::move(acceptor)
                      , [this, self]
                        (GenericStream c, Async yield_) {
                  auto yield = yield_.tag("frontend_u_s");
                  beast::flat_buffer c_rbuf;
                  Request rq;
                  if (auto r = http::async_read(c, c_rbuf, rq, yield.tag("read_req_u_s")); !r) {
                    return;
                  }

                  auto rs = fetch_fresh_from_front_end(rq, yield.tag("get_res_u_s"));

                  if (!rs) return;

                  http::async_write(c, *rs, yield.tag("write_res_u_s"));
            });
        });
    }

    setup_injectors();

    spawn_detached(
        _ctx.get_executor(),
        _shutdown_signal,
        _log_path.tag("setup_cache"),
        [this] (Async yield) {
            if (was_stopped()) return;
            auto r = setup_cache(yield);
            if (!r) LOG_ERROR(yield, " Failed to setup cache; ec=", r.error());
        }
    );

    if (auto cfg = _config.i2p_service_config()) {
        _i2p_service = I2pService::start(*cfg, _ctx.get_executor(), _shutdown_signal, _log_path);
    }
}

//------------------------------------------------------------------------------
unique_ptr<OuiServiceClient>
Client::State::maybe_wrap_tls(unique_ptr<OuiServiceClient> client)
{
    bool enable_injector_tls = !_config.tls_injector_cert_path().empty();

    if (!enable_injector_tls) {
        LOG_WARN(_log_path, "Connection to the injector shall not be encrypted");
        return client;
    }

    return make_unique<ouiservice::TlsOuiServiceClient>(std::move(client), inj_ctx);
}

void Client::State::setup_injectors()
{
    using R = SysResult<std::unique_ptr<OuiServiceClient>>;

    if (auto ep = _config.injector_endpoint<CacheType::Bep3HTTPOverI2P>()) {
        LOG_INFO(_log_path, " Setting up injector: ", *ep);

        struct Client : public OuiServiceClient {
            sys::error_code start(Async) override {
                return sys::error_code();
            }

            std::expected<GenericStream, sys::error_code>
            connect(Async yield) override {
                auto result = _session->connect(_addr, yield);
                if (!result) return std::unexpected(result.error());
                return std::move(*result);
            }

            Client(I2pAddress addr, std::shared_ptr<I2pSession> session, Cancel cancel, util::LogPath log_path):
                _addr(std::move(addr)),
                _session(std::move(session)),
                _cancel(std::move(cancel)),
                _log_path(std::move(log_path))
            {}

            ~Client() {
                _cancel();
            }

            I2pAddress _addr;
            std::shared_ptr<I2pSession> _session;
            Cancel _cancel;
            util::LogPath _log_path;
        };

        _injector_i2p = spawn_for_result(
                _ctx.get_executor(),
                _shutdown_signal,
                _log_path,
                [this, ep] (Async yield) -> R {
                auto session = get_or_create_i2p_session_task().wait(yield);

                if (!session) return std::unexpected(session.error());

                return std::make_unique<Client>(
                        *ep,
                        std::move(*session),
                        _shutdown_signal,
                        _log_path);
            });
    }

    auto injector_ep = _config.injector_endpoint<CacheType::Bep5Http>();

    if (injector_ep) {
        LOG_INFO(_log_path, " Setting up injector: ", *injector_ep);

        _injector_utp = spawn_for_result(_ctx.get_executor(), _shutdown_signal, _log_path,
            [this, injector_ep] (Async yield) -> R {
                assert(!yield.is_cancelled());
                auto client = injector_ep->visit(overloaded {
                    [&] (const asio::ip::tcp::endpoint& ep) -> R {
                        auto tcp_client = make_unique<ouiservice::TcpOuiServiceClient>(_ctx.get_executor(), ep);

                        if (!tcp_client->verify_endpoint()) {
                            return std::unexpected(asio::error::invalid_argument);
                        }
                        return maybe_wrap_tls(std::move(tcp_client));
                    },
                    [&] (const Endpoint::Utp& ep) -> R {
                        asio_utp::udp_multiplexer m(_ctx);
                        m.bind(common_udp_multiplexer());

                        auto utp_client = make_unique<ouiservice::UtpOuiServiceClient>
                            (_ctx.get_executor(), std::move(m), ep.value);

                        if (!utp_client->verify_remote_endpoint()) {
                            return std::unexpected(asio::error::invalid_argument);
                        }

                        return maybe_wrap_tls(std::move(utp_client));
                    },
                    [&] (const Endpoint::Bep5& ep) -> R {
                        auto dht = bittorrent_dht(yield);
                        if (!dht) {
                            LOG_ERROR("Failed to set up Bep5Client at setting up BT DHT; ec=", dht.error());
                            return std::unexpected(dht.error());
                        }

                        boost::optional<string> bridge_swarm_name = _config.bep5_bridge_swarm_name();

                        if (!bridge_swarm_name) {
                            LOG_ERROR("Bridge swarm name has not been computed");
                            return std::unexpected(asio::error::operation_not_supported);
                        }

                        auto client = make_unique<ouiservice::Bep5Client>(
                            *dht,
                            ep.value,
                            *bridge_swarm_name,
                            _config.is_bridge_announcement_enabled(),
                            &inj_ctx,
                            ouiservice::Bep5Client::injectors | ouiservice::Bep5Client::helpers,
                            _log_path
                        );

                        if (auto r = idempotent_start_accepting_on_utp(yield); !r) {
                            LOG_ERROR("Failed to start accepting on uTP; ec=", r.error());
                            return std::unexpected(r.error());
                        }

                        return client;
                    },
                    [] (const auto&) -> R {
                        return std::unexpected(asio::error::operation_not_supported);
                    }
                });

                if (!client) return std::unexpected(client.error());

                if (sys::error_code ec = (*client)->start(yield)) {
                    return std::unexpected(ec);
                }

                return std::move(*client);
            });
    }
}

//------------------------------------------------------------------------------
Client::Client(
        asio::io_context& ctx,
        ClientConfig cfg,
        util::LogPath log_path,
        std::optional<MockDhtBuilder> dht_builder)
    : _state(make_shared<State>(ctx, std::move(cfg), std::move(log_path), std::move(dht_builder)))
{
}

Client::~Client()
{
}

void Client::start()
{
    _state->start_ouinet();
}

void Client::stop()
{
    _state->stop_ouinet();
}

Client::RunningState Client::get_state() const noexcept {
    return _state->get_state();
}

asio::ip::tcp::endpoint Client::get_proxy_endpoint() const noexcept
{
    return _state->_proxy_endpoint;
}

std::string Client::get_frontend_endpoint() const noexcept
{
    return _state->_frontend_endpoint;
}

std::string Client::get_frontend_unix_socket_endpoint() const noexcept
{
    return _state->_frontend_unix_socket_endpoint;
}

AsioExecutor Client::get_executor() const noexcept
{
    return _state->_ctx.get_executor();
}

void Client::charging_state_change(bool is_charging) {
    LOG_DEBUG("Charging state changed, is charging: ", is_charging);
    //TODO do something
}

void Client::wifi_state_change(bool is_wifi_connected) {
    LOG_DEBUG("Wifi state changed, is connected: ", is_wifi_connected);
    //TODO do something
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

const ClientConfig& Client::config() const {
    return _state->_config;
}

std::shared_ptr<bt::DhtBase> Client::get_dht() const {
    return _state->_bt_dht;
}

bittorrent::NodeID Client::compute_infohash_for_resource_group(std::string_view group) const {
    auto cache_pub_key = *_state->_config.cache_http_pub_key();
    std::string uri_swarm_prefix = bep5::compute_uri_swarm_prefix(cache_pub_key, http_::protocol_version_current);
    std::string swarm_name = bep5::compute_uri_swarm_name(uri_swarm_prefix, {group.begin(), group.size()});
    util::SHA1::digest_type hash = util::sha1_digest(swarm_name);
    return bittorrent::NodeID(hash);
}

SysResult<I2pAddress> Client::local_i2p_address(Async yield) const {
    if (!_state->_i2p_session_create) return std::unexpected(asio::error::operation_not_supported);
    auto session = _state->_i2p_session_create->wait(yield);
    if (!session) return std::unexpected(session.error());
    return (*session)->local_addr();
}

//------------------------------------------------------------------------------
