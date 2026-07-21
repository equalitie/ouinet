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
#include "or_throw.h"
#include "request_routing.h"
#include "split_string.h"
#include "request.h"
#include "peer_message.h"
#include "full_duplex_forward.h"
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
#include "bittorrent/mainline_dht.h"

#include "ouiservice.h"
#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/util/create_i2p_session.h"
#include "ouiservice/tcp.h"
#include "ouiservice/utp.h"
#include "ouiservice/tls.h"
#include "ouiservice/weak_client.h"
#include "ouiservice/bep5/client.h"
#include "ouiservice/multi_utp_server.h"
#include "ouiservice/ouisync/ouisync.h"

#include "parse/number.h"
#include "util/cancel.h"
#include "util/lru_cache.h"
#include "util/scheduler.h"
#include "util/async_job.h"
#include "util/promise.h"
#include "upnp_updater.h"
#include "task.h"
#include "util/executor.h"

#include "task.h"
#include "logger.h"
#include "util/wait_condition.h"

#define _YDEBUG(y, ...) do { if (get_logger().get_threshold() <= DEBUG) y.log(DEBUG, __VA_ARGS__); } while (false)
#define _YWARN(y, ...) do { if (get_logger().get_threshold() <= WARN) y.log(WARN, __VA_ARGS__); } while (false)
#define _YERROR(y, ...) do { if (get_logger().get_threshold() <= ERROR_LEVEL) y.log(ERROR_LEVEL, __VA_ARGS__); } while (false)

using namespace std;
using namespace ouinet;

namespace posix_time = boost::posix_time;
namespace bt = ouinet::bittorrent;

using tcp      = asio::ip::tcp;
using Request  = http::request<http::string_body>;
using Response = http::response<http::dynamic_body>;
using TcpLookup = tcp::resolver::results_type;
using UdpEndpoints = std::set<asio::ip::udp::endpoint>;
using ouinet::util::AsioExecutor;

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

    using I2pSessionPromise = Promise<
            std::expected<
                std::shared_ptr<I2pSession>,
                I2pSession::Error::Create
            >
        >;
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
        , _injector_starting{get_executor()}
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
        if (_injector_starting) _injector_starting->notify(asio::error::shut_down);
        if (_cache_starting) _cache_starting->notify(asio::error::shut_down);

        _cache = nullptr;
        if (_upnps_ptr) _upnps_ptr->clear();
        _shutdown_signal();
        if (_injector) _injector->stop();
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
        bool use_injector(_config.injector_endpoint());
        bool use_cache(_config.is_cache_enabled());
        bool use_cache_bep5(use_cache && _config.is_cache_bep5());
        if (use_injector && _injector_starting)
            return Client::RunningState::Starting;
        if (use_cache && _cache_starting)
            return Client::RunningState::Starting;
        if (use_injector && _injector_start_ec)
            return Client::RunningState::Degraded;
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

    // All `fetch_*` functions below take care of keeping or dropping
    // Ouinet-specific internal HTTP headers as expected by upper layers.

    [[nodiscard]]
    std::expected<CacheEntry, sys::error_code>
    fetch_stored_in_dcache(const CacheRetrieveRequest& request, Async);


    [[nodiscard]]
    std::expected<Response, sys::error_code>
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
                                     , asio::ssl::context&
                                     , std::optional<metrics::Request>
                                     , Async);

    std::expected<Session, sys::error_code>
    fetch_fresh_through_simple_proxy( PublicInjectorRequest
                                    , metrics::Request
                                    , Async);

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

    std::expected<void, sys::error_code> setup_injector(Async);

    bool was_stopped() const {
        return (bool) _shutdown_signal;
    }

    inline
    std::expected<void, sys::error_code> wait_for_injector(Async yield) {
        while (true) {
            if (_injector) {
                return {};
            }

            if (!_injector_starting) {
                return std::unexpected(_injector_start_ec);
            }

            if (auto r = _injector_starting->wait(yield); !r) {
                return std::unexpected(r.error());
            }
        }
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

    asio::io_context& get_io_context() { return _ctx; }
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

    unique_ptr<OuiServiceImplementationClient>
    maybe_wrap_tls(unique_ptr<OuiServiceImplementationClient>);

    cache::Client* get_cache() const { return _cache.get(); }

    void serve_peer_request(GenericStream, Async);

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

    I2pSessionPromise::Future get_or_create_i2p_session_future(asio::any_io_executor exec) {
        if (!_i2p_session_future) {
            _i2p_session_future = create_i2p_session(_shutdown_signal, _log_path, exec);
        }
        return *_i2p_session_future;
    }

    std::shared_ptr<I2pSession> get_or_create_i2p_session(Async yield) {
        auto future = get_or_create_i2p_session_future(yield.get_executor());

        auto future_result = _i2p_session_future->wait(yield);
        if (!future_result.has_value()) {
            LOG_ERROR("Failed to create I2pSession: broken promise");
            return nullptr;
        }
        auto& create_result = future_result.value();
        if (!create_result.has_value()) {
            LOG_ERROR("Failed to create I2pSession: ", create_result.error());
            return nullptr;
        }
        return *create_result;
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
            auto slot = yield.cancel_slot([&] () mutable { _multi_utp_server = nullptr; });

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
                    serve_peer_request(std::move(con), yield);
                });
            }
        });

        return {};
    }

    void start_accepting_i2p(Async yield) {
        auto session = get_or_create_i2p_session(yield);
        if (!session) return;

        if (auto tracker_addr = _config.i2p_bep3_tracker()) {
            _cache->enable_i2p(session, *tracker_addr);
        }

        yield.spawn([this, session] (Async yield) mutable {
            while (true) {
                auto con = session->accept(yield);
                if (!con.has_value()) {
                    LOG_WARN("I2P cache: Failure to accept: ", con.error(), " is_open:", session->is_open());
                    async_sleep(200ms, yield);
                    continue;
                }
                LOG_INFO("Accepted I2P connection");
                yield.spawn([this, con = std::move(*con)] (Async yield) mutable {
                    serve_peer_request(std::move(con), yield.tag("serve_i2p_req"));
                });
            }
        });
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
    Cancel _shutdown_signal;

    // For debugging
    uint64_t _next_connection_id = 0;
    ConnectionPool<Endpoint> _injector_connections;
    ConnectionPool<bool> _self_connections;  // stored value is unused
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

    shared_ptr<ouiservice::Bep5Client> _bep5_client;

    shared_ptr<std::map<asio::ip::udp::endpoint, unique_ptr<UPnPUpdater>>> _upnps_ptr;
    metrics::Client _metrics;

    asio::ip::tcp::endpoint _proxy_endpoint;
    // _proxy_endpoint_address is a string version of _proxy_endpoint.
    std::string _proxy_endpoint_address;
    std::string _frontend_endpoint;
    std::optional<ouisync_service::Ouisync> _ouisync;
    std::string _frontend_unix_socket_endpoint;

    // This could be created either because of cache or injector
    std::optional<I2pSessionPromise::Future> _i2p_session_future;

    shared_ptr<dns::Resolver> _dns_resolver;
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
Client::State::serve_peer_request(GenericStream con, Async yield)
{
    Cancel cancel = _shutdown_signal;
    auto cancel_slot = cancel.connect([&] { con.close(); });

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

            auto wd = watch_dog(_ctx, rq_read_timeout, [&] { con.close(); });

            auto req_r = PeerRequest::async_read(con, yield.tag("read_req"));
            if (!req_r.has_value()) return;
            req = std::move(*req_r);
        }

        if (auto* cache_req = std::get_if<PeerCacheRequest>(&req)) {
            if (!_cache) {
                LOG_WARN(yield, " Received uTP request, but cache is not initialized");
                auto ec = send_error_response(con, cache_req->keep_alive(), http::status::not_found
                                             , "cache not initialized", yield);
                if (ec || !cache_req->keep_alive()) return;
                continue;
            }

            if(_cache->serve_local(
                        *cache_req,
                        con,
                        _metrics,
                        yield.tag("serve_local"))) {
                continue;
            }

            return;
        }

        auto connect_req = std::get_if<PeerConnectRequest>(&req);

        auto cyield = yield.tag("connect");

        if (!connect_req) {
            handle_bad_request( con, false, "Invalid request", cyield.tag("invalid request"));
            return;
        }

        LOG_DEBUG(cyield, " Client: Received uTP/CONNECT request");

        // Connect to the injector and tunnel the transaction through it

        if (!_bep5_client) {
            handle_bad_request( con, false, "No known injectors"
                              , cyield.tag("handle_no_injectors_error"));
            return;
        }

        auto inj = _bep5_client->connect( cyield.tag("connect_to_injector")
                                        , false, ouiservice::Bep5Client::injectors);

        if (!inj.has_value()) {
            handle_bad_request( con, false, "Failed to connect to injector"
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

        ec = util::http_reply(con, res, cyield.tag("write_res"));
        if (ec) return;

        // Forward the rest of data in both directions.
        ec = full_duplex(
            std::move(con),
            std::move(*inj),
            [&] (size_t byte_count) { fwd_bytes_c2i += byte_count; _metrics.bridge_transfer_c2i(byte_count); },
            [&] (size_t byte_count) { fwd_bytes_i2c += byte_count; _metrics.bridge_transfer_i2c(byte_count); },
            cyield.tag("full_duplex"));

        return;
    }
}

//------------------------------------------------------------------------------
std::expected<CacheEntry, sys::error_code>
Client::State::fetch_stored_in_dcache(const CacheRetrieveRequest& request, Async yield)
{
    try {
        Async timeout_yield = yield;
        auto watch_dog = ouinet::watch_dog( _ctx
                                          , default_timeout::fetch_http()
                                          , [&]{ timeout_yield.cancel(); });

        if (_config.cache_type() == ClientConfig::CacheType::Bep5Http) {
            if (auto r = wait_for_cache(timeout_yield); !r) {
                return std::unexpected(r.error());
            }
        }

        auto c = get_cache();

        auto get_date = [](auto& hdr) {
            auto tsh = util::http_injection_ts(hdr);
            auto ts = parse::number<time_t>(tsh);
            return ts ? boost::posix_time::from_time_t(*ts)
                      : boost::posix_time::not_a_date_time;
        };

        if (c && (_config.cache_type() == ClientConfig::CacheType::Bep5Http
               || _config.cache_type() == ClientConfig::CacheType::Bep3HTTPOverI2P)) {
            auto rq = request.to_peer_request();
            auto key = rq.resource_id();

            auto s = c->load( request.resource_id()
                            , request.resource_key()
                            , rq.dht_group()
                            , rq.method() == http::verb::head
                            , _metrics
                            , timeout_yield.tag("load"));

            if (!s) return std::unexpected(s.error());

            auto& hdr = s->response_header();

            if (!util::http_proto_version_check_trusted(hdr, newest_proto_seen))
                // The cached resource cannot be used, treat it like
                // not being found.
                return std::unexpected(asio::error::not_found);

            maybe_add_proto_version_warning(hdr);
            assert(!hdr[http_::response_source_hdr].empty());  // for agent, set by cache
            auto date = get_date(hdr);
            return CacheEntry{date, std::move(*s)};
        }
        else if(_ouisync && _ouisync->is_running() && _config.cache_type() == ClientConfig::CacheType::Ouisync) {
            auto rq = request.to_ouisync_request();
            auto session = _ouisync->load(rq, yield);
            if (!session) return std::unexpected(session.error());
            auto date = get_date(session->response_header());
            return CacheEntry{date, std::move(*session)};
        }
        else {
            LOG_DEBUG(yield, " Cache is disabled");
            return std::unexpected(asio::error::operation_not_supported);
        }
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
std::expected<Response, sys::error_code>
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

    auto res = _front_end.serve( _config
                               , rq
                               , get_state()
                               , _cache.get()
                               , _bep5_client
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

            // Connect to the injector/proxy.
            if (auto result = wait_for_injector(yield); !result) {
                return std::unexpected(result.error());
            }
            assert(_injector);

            auto inj_e = _injector->connect(yield);
            if (!inj_e) {
                if (metrics) metrics->finish(inj_e.error());
                return std::unexpected(inj_e.error());
            }
            auto inj = std::move(*inj_e);

            // Build the actual request to send to the proxy.
            Request connreq = { http::verb::connect
                                , url->host + ":" + (url->port.empty() ? "443" : url->port)
                                , 11 /* HTTP/1.1 */};

            // HTTP/1.1 requires a ``Host:`` header in all requests:
            // <https://tools.ietf.org/html/rfc7230#section-5.4>.
            connreq.set(http::field::host, connreq.target());

            if (auto credentials = _config.credentials_for(inj.remote_endpoint))
                authorize(connreq, *credentials);

            // Open a tunnel to the origin
            // (to later perform the SSL handshake and send the request).
            connreq.prepare_payload();


            auto req_e = util::http_request(inj.connection, connreq, yield.tag("connreq"));

            if (!req_e) {
                if (metrics) metrics->finish(req_e.error());
                return std::unexpected(req_e.error());
            }

            // Only get the head of the CONNECT response
            // (otherwise we would get stuck waiting to read
            // a body whose length we do not know
            // since a successful respone should have no content length as per RFC7231#4.3.6).
            {
                auto r = std::make_unique<http_response::Reader>(std::move(inj.connection));

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

                inj.connection = r->release_stream();
            }

            std::expected<GenericStream, sys::error_code> con_e;
            if (url->scheme == "https") {
                con_e = ssl::util::client_handshake( std::move(inj.connection)
                                                   , tls_ctx
                                                   , url->host
                                                   , yield);
            } else {
                con_e = std::move(inj.connection);
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
std::expected<Session, sys::error_code>
Client::State::fetch_fresh_through_simple_proxy( PublicInjectorRequest request
                                               , metrics::Request metrics
                                               , Async yield)
{
    return timeout(
        default_timeout::fetch_http(),
        [&](Async yield) -> std::expected<Session, sys::error_code> {
            // Connect to the injector.
            // TODO: Maybe refactor with `fetch_via_self`.

            if (auto result = wait_for_injector(yield); !result) {
                return std::unexpected(result.error());
            }
            assert(_injector);

            ConnectionPool<Endpoint>::Connection con;
            if (_injector_connections.empty()) {
                LOG_DEBUG(yield, " Connecting to the injector");

                auto connect_e = _injector->connect(yield);

                if (!connect_e) {
                    LOG_WARN(yield, " Failed to connect to injector; ec=", connect_e.error());
                    metrics.finish(connect_e.error());
                    return std::unexpected(connect_e.error());
                }

                auto connect = std::move(*connect_e);

                assert(connect.connection.has_implementation());

                con = _injector_connections.wrap(std::move(connect.connection));
                *con = connect.remote_endpoint;
            } else {
                LOG_DEBUG(yield, " Reusing existing injector connection");
                con = _injector_connections.pop_front();
            }

            auto cancel_slot = yield.cancel_slot([&] { con.close(); });

            if (auto credentials = _config.credentials_for(*con)) {
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
            auto write_e = compat([&](asio::yield_context yield) {
                return request.async_write(con, yield);
            })(yield.tag("write_injector_req"));
            if (!write_e) {
                LOG_WARN(yield, " Failed to send request to the injector; ec=", write_e.error());
                metrics.finish(write_e.error());
                return std::unexpected(write_e.error());
            }

            LOG_DEBUG(yield, " Reading response");

            cancel_slot = {};

            // Receive response
            auto session_e = Session::create(
                std::move(con),
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
    auto injector_session = fetch_fresh_through_connect_proxy(req, tls_ctx, {}, yield);

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

//------------------------------------------------------------------------------
class Transaction {
public:
    Transaction(GenericStream& ua_con, const Request& rq)
        : _ua_con(ua_con)
        , _request(rq)
    {}

    std::expected<void, sys::error_code>
    write_to_user_agent(Session& session, Async yield)
    {
        namespace err = asio::error;

        if (yield.is_cancelled()) {
            assert(false);
            LOG_ERROR(__FILE__, ":", __LINE__, " Cancel already called");
            return std::unexpected(err::operation_aborted);
        }

        if (_ua_was_written_to) {
            return std::unexpected(err::already_started);
        }

        _ua_was_written_to = true;

        // Using PartModifier::RemoveChunkHeaderExtension because the WebKit on
        // iOS can't handle the extension string in chunk headers.
        auto r = session.flush_response(_ua_con, yield, PartModifier::RemoveChunkHeaderExtension);

        if (r) {
            _response_header = session.response_header();
        }

        bool keep_alive = r && _request.keep_alive() && session.keep_alive();

        if (!keep_alive) {
            session.close();
            _ua_con.close();
        }

        if (!r) return std::unexpected(r.error());
        return {};
    }

    template<class BodyType>
    std::expected<void, sys::error_code>
    write_to_user_agent(const http::response<BodyType>& rs, Async yield)
    {
        namespace err = asio::error;

        if (yield.is_cancelled()) {
            LOG_ERROR(__FILE__, ":", __LINE__, " Cancel already called");
            return std::unexpected(err::operation_aborted);
        }

        if (_ua_was_written_to) {
            return std::unexpected(err::already_started);
        }

        _ua_was_written_to = true;
        auto write_r = http::async_write(_ua_con, rs, yield);

        if (write_r) {
            _response_header = rs.base();
        }

        bool keep_alive = write_r && _request.keep_alive() && rs.keep_alive();

        if (!keep_alive) _ua_con.close();

        if (!write_r) return std::unexpected(write_r.error());

        return {};
    }

    const Request& request() const { return _request; }

    bool user_agent_was_written_to() {
        return _ua_was_written_to;
    }

    bool is_open() const {
        return _ua_con.is_open();
    }

    http::response_header<> const*  response_header() const {
        if (!_response_header) return nullptr;
        return &*_response_header;
    }

private:
    /*
     * Connection to the user agent
     */
    GenericStream& _ua_con;
    const Request& _request;
    bool _ua_was_written_to = false;
    std::optional<http::response_header<>> _response_header;
};

//------------------------------------------------------------------------------
class Client::ClientCacheControl {
public:
    ClientCacheControl(Client::State& client_state)
        : client_state(client_state)
        , cache_control(client_state.get_executor(), OUINET_CLIENT_SERVER_STRING)
    {
        //------------------------------------------------------------
        cache_control.fetch_fresh = [&] ( const CacheInjectRequest& rq
                                        , Async yield_) -> std::expected<Session, sys::error_code>
        {
            auto yield = yield_.tag("injector");

            namespace err = asio::error;

            LOG_DEBUG(yield, " Start");

            if (!client_state._config.is_injector_access_enabled()) {
                LOG_DEBUG(yield, " Disabled");
                return std::unexpected(err::operation_not_supported);
            }

            auto metrics = client_state._metrics.new_public_injector_request();

            auto session = client_state.fetch_fresh_through_simple_proxy(
                rq,
                std::move(metrics),
                yield
            );

            if (!session) {
                LOG_DEBUG(yield, " Finish with error: ", session.error());
            } else {
                LOG_DEBUG(yield, " Finish successfully; status="
                               , session->response_header().result());
            }

            return session;
        };

        //------------------------------------------------------------
        cache_control.fetch_stored = [&] (const CacheRetrieveRequest& rq, Async yield_) {
            auto yield = yield_.tag("cache");

            LOG_DEBUG(yield, " Start");

            auto entry = client_state.fetch_stored_in_dcache(rq, yield);

            if (!entry) {
                LOG_DEBUG(yield, " Finish with error: ", entry.error());
            } else {
                LOG_DEBUG(yield, " Finish successfully");
            }

            return entry;
        };

        //------------------------------------------------------------
        cache_control.max_cached_age(client_state._config.max_cached_age());
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    front_end_job_func(Transaction& tnx, Async yield) {
        auto res = client_state.fetch_fresh_from_front_end(tnx.request(), yield);
        if (!res) return std::unexpected(res.error());
        if (auto r = tnx.write_to_user_agent(*res, yield); !r) {
            return std::unexpected(r.error());
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    origin_job_func(Transaction& tnx, Async yield) {
        if (yield.get_cancel()) {
            LOG_ERROR(yield, " origin_job_func received an already triggered cancel");
            return std::unexpected(asio::error::operation_aborted);
        }

        LOG_DEBUG(yield, " Start");

        // Avoid leaking to non-injectors
        auto rq = tnx.request();
        util::remove_ouinet_fields_ref(rq);

        auto metrics = client_state._metrics.new_origin_request();

        sys::error_code ec;
        auto session = client_state.fetch_fresh_from_origin( rq
                                                           , client_state._config.origin_ssl_ctx()
                                                           , std::move(metrics)
                                                           , yield);

        if (!session) {
            LOG_WARN(yield, " Fetch; ec=", ec);
            return std::unexpected(session.error());
        }

        if (auto r = tnx.write_to_user_agent(*session, yield); !r) {
            LOG_WARN(yield, " Flush; ec=", r.error());
        }

        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    proxy_job_func(Transaction& tnx, Async yield) {

        LOG_DEBUG(yield, "Start");

        std::expected<Session, sys::error_code> session;

        auto rq = tnx.request();

        if (rq.target().starts_with("https://")) {
            auto metrics = client_state._metrics.new_private_injector_request();

            util::remove_ouinet_fields_ref(rq);

            session = client_state.fetch_fresh_through_connect_proxy(
                rq,
                client_state._config.origin_ssl_ctx(),
                std::move(metrics),
                yield.tag("connect")
            );
        }
        else {
            auto metrics = client_state._metrics.new_public_injector_request();

            auto insecure_rq = InsecureRequest::from(std::move(rq));

            if (!insecure_rq) {
                return std::unexpected(asio::error::invalid_argument);
            }

            session = client_state.fetch_fresh_through_simple_proxy(
                std::move(*insecure_rq),
                std::move(metrics),
                yield.tag("simple")
            );
        }

        LOG_DEBUG(yield, "Proxy fetch; ec=", session ? sys::error_code() : session.error());

        auto r = tnx.write_to_user_agent(*session, yield);

        LOG_DEBUG(yield, "Flush; ec=", r ? sys::error_code() : r.error());

        if (!r) return std::unexpected(r.error());

        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code> injector_job_func(Transaction& tnx, Async yield) {
        namespace err = asio::error;

        LOG_DEBUG(yield, " Start");

        const auto rq = CacheRequest::from(tnx.request());
        if (!rq) {
            LOG_ERROR(yield, " Invalid request");
            return std::unexpected(asio::error::invalid_argument);
        }

        auto session = cache_control.fetch(*rq, yield.tag("cc_fetch"));
        LOG_DEBUG(yield.tag("cc_fetch"), " Done; ec=", session ? sys::error_code{} : session.error());

        if (!session) return std::unexpected(session.error());

        auto& rsh = session->response_header();

        auto injector_error = rsh[http_::response_error_hdr];
        if (!injector_error.empty()) {
            LOG_ERROR(yield, " Error from injector: ", injector_error);
            auto r = tnx.write_to_user_agent(*session, yield);
            if (!r) return std::unexpected(r.error());
            return {};
        }

        auto exec = yield.get_executor();

        using http_response::Part;

        util::AsyncQueue<std::optional<Part>> qst(exec), qag(exec); // to storage, agent

        WaitCondition wc(exec);

        auto cache = client_state.get_cache();

        const char* no_cache_reason = nullptr;
        bool do_cache =
            ( cache
            && rq->header().method() == http::verb::get  // TODO: storing HEAD response not yet supported
            && rsh[http_::response_source_hdr] != http_::response_source_hdr_local_cache
            && CacheControl::ok_to_cache( rq->header(), rsh, client_state._config.do_cache_private()
                                        , (get_logger().get_threshold() <= DEBUG ? &no_cache_reason : nullptr)));

        if (do_cache) {
            yield.spawn([ &, cache = std::move(cache), lock = wc.lock() ] (Async yield) {
                auto key = rq->resource_id();
                AsyncQueueReader rr(qst);
                auto r = cache->store(key, rq->dht_group(), rr, yield);
                if (!r) LOG_ERROR(yield, " Failed to write response to cache; ec=", r.error());
            });
        } else {
            LOG_DEBUG( yield, " Not ok to cache response: "
                   , no_cache_reason
                         ? no_cache_reason
                         : (!cache ? "cache not available"
                                   : "disabled for this request/response"));
        }

        yield.spawn([&, lock = wc.lock() ] (Async yield) {
            auto rr = std::make_unique<AsyncQueueReader>(qag);
            auto sag = Session::create(std::move(rr), tnx.request().method() == http::verb::head, yield);
            if (!sag) return;
            auto r = tnx.write_to_user_agent(*sag, yield);
            if (!r) LOG_ERROR(yield, " Failed to write response to user agent; ec=", r.error());
        });

        auto r = session->flush_response(yield.tag("flush"),
            [&] (Part&& part, Async yield) -> std::expected<void, sys::error_code>
            {
                // If the user agent closed its connection, stop getting data from the injector too.
                // Otherwise, besides continuing to transfer data to the local cache,
                // it will also accumulate in memory (at the `qag` queue, which is no longer read),
                // with both being especially problematic with big resources like videos.
                //
                // Please note that this will cause an incomplete response to be stored;
                // hopefully the Injector mechanism may be faster to respond
                // if the client tries to download the same resource again.
                // Another fix would be to have the local cache participate in multi-peer downloads.
                if (!tnx.is_open()) {
                    return std::unexpected(asio::error::broken_pipe);
                }
                if (do_cache) qst.push_back(part);
                qag.push_back(std::move(part));
                return {};
            },
            default_timeout::activity());

        if (do_cache) qst.push_back(std::nullopt);
        qag.push_back(std::nullopt);

        // Wait for the spawned tasks to finish
        wc.wait(yield.tag("wait"));

        LOG_DEBUG(yield, " Finish; ec=", r ? sys::error_code() : r.error());

        if (!r) return std::unexpected(r.error());

        return {};
    }


    struct Jobs {
        enum class Type {
            front_end,
            origin,
            proxy,
            injector_or_dcache
        };

        using Job = AsyncJob<void>;
        using BoolFunc = std::function<bool(void)>;

        Jobs(AsioExecutor exec, BoolFunc is_injector_starting)
            : exec(exec)
            , front_end(exec)
            , origin(exec)
            , proxy(exec)
            , injector_or_dcache(exec)
            , all({&front_end, &origin, &proxy, &injector_or_dcache})
            , is_injector_starting{std::move(is_injector_starting)}
        {}

        AsioExecutor exec;

        Job front_end;
        Job origin;
        Job proxy;
        Job injector_or_dcache;

        // All jobs, even those that never started.
        // Unfortunately C++14 is not letting me have array of references.
        const std::array<Job*, 4> all;

        BoolFunc is_injector_starting;

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

        void sleep_before_job(Type job_type, Async yield) {
            size_t n = count_running();

            // 'n' includes "this" job, and we don't need to wait for that.
            assert(n > 0);
            if (n > 0) --n;

            if (job_type == Type::injector_or_dcache || job_type == Type::proxy) {
                // If origin is running, give it some time, but stop sleeping
                // if origin fetch exits early.
                if (!origin.is_running()) return;

                std::optional<Job::Connection> jc;

                if (origin.is_running()) {
                    jc = origin.on_finish_sig([&] { yield.cancel(); });
                }

                // If the injector is still starting, push injector/cache job a little earlier
                // (reducing the latency of local cache use)
                // since connectivity may be missing and origin will eventually fail.
                auto delay = (job_type == Type::injector_or_dcache && is_injector_starting())
                    ? n * chrono::seconds(1)
                    : n * chrono::seconds(3);

                async_sleep(delay, yield);
            } else if (job_type == Type::front_end) {
                // No pause for front-end jobs.
            } else {
                async_sleep(n * chrono::seconds(3), yield);
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
    [[nodiscard]]
    std::expected<void, sys::error_code>
    mixed_fetch(Transaction& tnx, const request_route::Config& request_config, Async yield)
    {
        Cancel cancel(client_state._shutdown_signal);

        namespace err = asio::error;

        using request_route::fresh_channel;

        using Job = Jobs::Job;
        using JobCon = Job::Connection;
        using OptJobCon = std::optional<JobCon>;

        auto exec = yield.get_executor();

        Jobs jobs(exec, [&] { return bool(client_state._injector_starting); });

        auto cancel_con = cancel.connect([&] {
            for (auto& job : jobs.running()) job.cancel();
        });

        auto start_job = [&] (Jobs::Type job_type, auto func) {
            const char* name_tag = Jobs::as_string(job_type);

            Job* job = jobs.job_from_type(job_type);

            assert(job); if (!job) return;

            if (!is_access_enabled(job_type)) {
                LOG_DEBUG(yield, " ", name_tag, ": disabled");
                return;
            }

            job->start([
                &jobs,
                name_tag,
                func = std::move(func),
                job_type
            ] (Async y) {
                jobs.sleep_before_job(job_type, y.tag(name_tag));
                return func(y);
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
                            [&] (auto y)
                            { return front_end_job_func(tnx, y); });
                    break;
                }
                case fresh_channel::origin: {
                    start_job(Jobs::Type::origin,
                            [&] (auto y)
                            { return origin_job_func(tnx, y); });
                    break;
                }
                case fresh_channel::proxy: {
                    start_job(Jobs::Type::proxy,
                            [&] (auto y)
                            { return proxy_job_func(tnx, y); });
                    break;
                }
                case fresh_channel::injector_or_dcache: {
                    start_job(Jobs::Type::injector_or_dcache,
                            [&] (auto y)
                            { return injector_job_func(tnx, y); });
                    break;
                }
            }
        }

        const char* final_job = "(none)";
        boost::optional<sys::error_code> final_ec;

        auto target = tnx.request().target();
        std::string short_target = std::string(target.substr(0, 64));
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

            LOG_DEBUG(yield, " Waiting for ", job_count, " running jobs");

            cv.wait(yield);

            if (!which) {
                LOG_WARN(yield, " Got result from unknown job");
                continue; // XXX
            }

            auto&& result = which->result();

            LOG_DEBUG( yield, " Got result; job=", jobs.as_string(which)
                   , " ec=", (result ? sys::error_code() : result.error())
                   , " target=", short_target);

            if (auto h = tnx.response_header()) {
                LOG_DEBUG(yield, *h);
            }

            if (result) {
                final_job = jobs.as_string(which);
                final_ec = sys::error_code{}; // success
                for (auto& job : jobs.running()) {
                    job.stop(yield);
                }
                break;
            } else if (!final_ec) {
                final_job = jobs.as_string(which);
                final_ec = result.error();
            }
        }

        if (!final_ec /* not set */) {
            final_ec = err::no_protocol_option;
        }

        LOG_DEBUG( yield, " Done; final_job=", final_job, " final_ec=", *final_ec
               , " target=", short_target);

        if (*final_ec) return std::unexpected(*final_ec);
        return {};
    }

private:
    Client::State& client_state;
    CacheControl cache_control;
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
    Response rs;
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

    namespace rr = request_route;
    using rr::fresh_channel;

    auto close_con_slot = _shutdown_signal.connect([&con] {
        con.close();
    });

    Client::ClientCacheControl cache_control(*this);

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

        auto request_config = request_route::route_choose_config(req, _config);

        Transaction tnx(con, req);

        if (request_config.fresh_channels.empty()) {
            LOG_DEBUG(yield, " Abort due to no route");
            auto r = tnx.write_to_user_agent( retrieval_failure_response(req), yield);
            if (!r) break;
            continue;
        }

        auto r = cache_control.mixed_fetch(tnx, request_config, yield.tag("mixed_fetch"));

        if (!r) {
            LOG_ERROR(yield, " Error writing back response; ec=", r.error());

            if (tnx.user_agent_was_written_to())
                con.close();  // it may already be closed
            if (con.is_open() && !cancel) {
                std::ignore = tnx.write_to_user_agent( retrieval_failure_response(req), yield);
            }
            if (!req.keep_alive())
                con.close();
        }

        if (!con.is_open()) {
            break;
        }
    }

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

    if (_config.cache_type() != ClientConfig::CacheType::Bep5Http
        && _config.cache_type() != ClientConfig::CacheType::Bep3HTTPOverI2P)
    {
        //unsupported cache type
        return std::unexpected(asio::error::operation_not_supported);
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

    if (_config.cache_type() == ClientConfig::CacheType::Bep5Http) {
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
    else if (_config.cache_type() == ClientConfig::CacheType::Bep3HTTPOverI2P) {
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

            task::spawn_detached( _ctx, [
                this,
                self = shared_from_this(),
                c = std::move(connection),
                handler,
                lock = wait_condition.lock()
            ](asio::yield_context yield) mutable {
                if (was_stopped()) return;
                handler(std::move(c), Async(yield, _shutdown_signal, _log_path));
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

            task::spawn_detached( _ctx, [
                this,
                self = shared_from_this(),
                c = std::move(connection),
                handler,
                lock = wait_condition.lock()
            ](asio::yield_context yield) mutable {
                if (was_stopped()) return;
                handler(std::move(c), Async(yield, _shutdown_signal, util::LogPath("unix_socket")));
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
        task::spawn_detached(_ctx, [
            this,
            self = shared_from_this()
        ] (asio::yield_context yield) mutable {
            sys::error_code ec = _ouisync->start(Async(yield, _shutdown_signal, _log_path));

            if (!ec) {
                LOG_INFO("Ouisync started");
            }
            else {
                LOG_ERROR("Failed to start Ouisync: ", ec.message());
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

    task::spawn_detached(_ctx, [
        this
    ] (asio::yield_context yield) {
        if (was_stopped()) return;

        sys::error_code ec;

        try {
            auto r = setup_injector(Async(yield, _shutdown_signal, _log_path.tag("setup_injector")));
            if (!r) ec = r.error();
        }
        catch (Async::Cancelled const&) {
            ec = asio::error::operation_aborted;
        }

        if (ec && ec != asio::error::operation_aborted)
            LOG_ERROR("Failed to setup injector; ec=", ec);

        if (_injector_starting) {
            _injector_start_ec = ec;
            _injector_starting->notify(ec);
            _injector_starting.reset();
        }
    });

    task::spawn_detached(_ctx, [this] (asio::yield_context yield) {
        if (was_stopped()) return;
        auto r = setup_cache(Async(yield, _shutdown_signal, _log_path.tag("setup_cache")));
        if (!r) LOG_ERROR("Failed to setup cache; ec=", r.error());
    });
}

//------------------------------------------------------------------------------
unique_ptr<OuiServiceImplementationClient>
Client::State::maybe_wrap_tls(unique_ptr<OuiServiceImplementationClient> client)
{
    bool enable_injector_tls = !_config.tls_injector_cert_path().empty();

    if (!enable_injector_tls) {
        LOG_WARN(_log_path, "Connection to the injector shall not be encrypted");
        return client;
    }

    return make_unique<ouiservice::TlsOuiServiceClient>(std::move(client), inj_ctx);
}

std::expected<void, sys::error_code> Client::State::setup_injector(Async yield)
{
    // Remember to always set before return in case of error,
    // or the notification may not pass the right error code to listeners.
    auto injector_ep = _config.injector_endpoint();
    if (!injector_ep) {
        return std::unexpected(asio::error::operation_not_supported);
    }

    LOG_INFO("Setting up injector: ", *injector_ep);

    std::unique_ptr<OuiServiceImplementationClient> client;

    if (auto ep = injector_ep->get_if<I2pAddress>()) {
        struct Client : public ouinet::OuiServiceImplementationClient {
            sys::error_code start(Async) override {
                return sys::error_code();
            }

            void stop() override {
                _cancel();
            }

            std::expected<GenericStream, sys::error_code>
            connect(Async yield) override {
                auto future_result = _i2p_session_future.wait(yield);

                if (!future_result.has_value()) {
                    return std::unexpected(asio::error::fault);
                }
                auto& create_result = future_result.value();
                if (!create_result.has_value()) {
                    return std::unexpected(asio::error::fault);
                }
                auto session = *create_result;
                auto result = session->connect(_addr, yield);
                if (!result.has_value()) {
                    return std::unexpected(result.error().code());
                }
                return std::move(*result);
            }

            Client(I2pAddress addr, I2pSessionPromise::Future i2p_session_future, Cancel cancel, util::LogPath log_path):
                _addr(std::move(addr)),
                _i2p_session_future(std::move(i2p_session_future)),
                _cancel(std::move(cancel)),
                _log_path(std::move(log_path))
            {}

            I2pAddress _addr;
            I2pSessionPromise::Future _i2p_session_future;
            Cancel _cancel;
            util::LogPath _log_path;
        };
        client = std::make_unique<Client>(*ep, get_or_create_i2p_session_future(yield.get_executor()), _shutdown_signal, _log_path);
    }
    else if (auto ep = injector_ep->get_if<asio::ip::tcp::endpoint>()) {
        auto tcp_client = make_unique<ouiservice::TcpOuiServiceClient>(_ctx.get_executor(), *ep);

        if (!tcp_client->verify_endpoint()) {
            return std::unexpected(asio::error::invalid_argument);
        }
        client = maybe_wrap_tls(std::move(tcp_client));
    } else if (auto ep = injector_ep->get_if<Endpoint::Utp>()) {
        asio_utp::udp_multiplexer m(_ctx);
        m.bind(common_udp_multiplexer());

        auto utp_client = make_unique<ouiservice::UtpOuiServiceClient>
            (_ctx.get_executor(), std::move(m), ep->value);

        if (!utp_client->verify_remote_endpoint()) {
            return std::unexpected(asio::error::invalid_argument);
        }

        client = maybe_wrap_tls(std::move(utp_client));
    } else if (auto ep = injector_ep->get_if<Endpoint::Bep5>()) {
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

        _bep5_client = make_shared<ouiservice::Bep5Client>(
            *dht,
            ep->value,
            *bridge_swarm_name,
            _config.is_bridge_announcement_enabled(),
            &inj_ctx,
            ouiservice::Bep5Client::injectors | ouiservice::Bep5Client::helpers,
            _log_path
        );

        client = make_unique<ouiservice::WeakOuiServiceClient>(_bep5_client);

        if (auto r = idempotent_start_accepting_on_utp(yield); !r) {
            LOG_ERROR("Failed to start accepting on uTP; ec=", r.error());
        }
    }

    _injector = std::make_unique<OuiServiceClient>(_ctx.get_executor());
    _injector->add(*injector_ep, std::move(client));

    if (sys::error_code ec = _injector->start(yield)) {
        return std::unexpected(ec);
    }

    return {};
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

//------------------------------------------------------------------------------
