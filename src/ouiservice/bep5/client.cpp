#include <boost/functional/hash.hpp>

#include "client.h"
#include "../utp.h"
#include "../connect_proxy.h"
#include "../tls.h"
#include "../../async_sleep.h"
#include "../../bittorrent/mainline_dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../logger.h"
#include "../../util/hash.h"
#include "../../util/lru_cache.h"
#include "../../ssl/util.h"
#include "../../task.h"
#include "../../util/wait_condition.h"
#include "../../util/watch_dog.h"
#include "../../util/select.h"
#include "../../util/semaphore.h"
#include <boost/asio/experimental/channel.hpp>

// It is ok to have many of these as a resort if injectors are not reachable,
// as long as they are fresh in the DHT.
static const std::size_t helper_swarm_capacity = 100;
// It is probably good to drop entries more aggressively from here
// to avoid accumulating spurious fake injector entries
// which may impede trying to ping good injectors.
static const std::size_t injector_swarm_capacity = 50;
// Choose values which would allow trying to ping a single injector entry
// if it was always available in the DHT,
// before we go over the "questionable" period (15 minutes according to BEP5)
// several times in a row.
static const std::size_t injectors_to_ping = 30;
static const auto injector_ping_period = std::chrono::minutes(10);
static const auto injector_ping_period_debug = std::chrono::minutes(2);
static const auto injector_pong_timeout = std::chrono::seconds(60);

using namespace ouinet;
using namespace ouiservice;
using namespace std::chrono_literals;

namespace bt = bittorrent;

using udp = asio::ip::udp;
using Clock = std::chrono::steady_clock;

static bool same_ipv(const udp::endpoint& ep1, const udp::endpoint& ep2)
{
    return ep1.address().is_v4() == ep2.address().is_v4();
}

static
boost::optional<asio_utp::udp_multiplexer>
choose_multiplexer_for(bt::DhtBase& dht, const udp::endpoint& ep)
{
    auto eps = dht.local_endpoints();

    for (auto& e : eps) {
        if (!same_ipv(ep, e)) continue;

        asio_utp::udp_multiplexer m(dht.get_executor());
        sys::error_code ec;
        m.bind(e, ec);
        assert(!ec);

        return m;
    }

    return boost::none;
}

constexpr std::chrono::duration ERROR_WAIT_DURATION = 1s;
constexpr std::chrono::duration SUCCESS_WAIT_DURATION = 1min;

struct Bep5Client::Swarm
{
private:
    using Peer  = OuiServiceClient;
    using Peers = util::LruCache<asio::ip::udp::endpoint, std::shared_ptr<Peer>>;

private:
    std::shared_ptr<bt::DhtBase> _dht;
    bt::NodeID _infohash;
    SwarmType _type;
    Cancel _lifetime_cancel;
    std::optional<std::chrono::steady_clock::time_point> _last_success_time;
    std::vector<WaitCondition::Lock> _wait_condition_locks;
    Peers _peers;
    const bool _connect_proxy;
    util::LogPath _log_path;

public:
    Swarm( bt::NodeID infohash
         , std::shared_ptr<bt::DhtBase> dht
         , size_t capacity
         , SwarmType type
         , Cancel& cancel
         , bool connect_proxy
         , util::LogPath log_path)
        : _dht(std::move(dht))
        , _infohash(infohash)
        , _type(type)
        , _lifetime_cancel(cancel)
        , _peers(capacity)
        , _connect_proxy(connect_proxy)
        , _log_path(std::move(log_path))
    {}

    ~Swarm() {
        _wait_condition_locks.clear();
        _lifetime_cancel();
    }

    void start() {
        task::spawn_detached(_dht->get_executor(), [&] (asio::yield_context y) {
            loop(Async(y, _lifetime_cancel, _log_path));
        });
    }

    const Peers& peers() const { return _peers; }

    std::vector<Candidate> candidates() const {
            std::vector<Candidate> ret;
            ret.reserve(_peers.size());
            for (auto& p : _peers) ret.push_back({p.first, p.second, _type});
            return ret;
    }

    bool is_ready() const {
        if (!_last_success_time) return false;
        auto duration = std::chrono::steady_clock::now() - *_last_success_time;
        return duration < 5 * SUCCESS_WAIT_DURATION;
    }

    void wait_for_ready(Async yield) {
        if (is_ready()) return;
        WaitCondition wc(_dht->get_executor());
        _wait_condition_locks.push_back(wc.lock());
        wc.wait(yield);
    }

    AsioExecutor get_executor() { return _dht->get_executor(); }

    friend std::ostream& operator << (std::ostream& os, const Swarm& swarm) {
        return os << swarm._type << "/" << swarm._infohash;
    }

private:
    void loop(Async yield) {
        while (true) {
            _dht->wait_all_ready(yield);

            LOG_DEBUG(yield, " Looking for peers (infohash=", _infohash, ")...");
            auto endpoints = _dht->tracker_get_peers(_infohash, yield);
            if (!endpoints || endpoints->empty()) {
                LOG_DEBUG(yield, " Looking for peers (infohash=", _infohash
                               , ") done: 0 found. Retry in ", ERROR_WAIT_DURATION);
                async_sleep(ERROR_WAIT_DURATION, yield);
                continue;
            }

            LOG_DEBUG(yield, " Looking for peers (infohash=", _infohash
                           , ") done: ", endpoints->size(), " found");


            add_peers(std::move(*endpoints));

            // If no peers were added then we're still not ready
            if (_peers.empty()) continue;

            _last_success_time = std::chrono::steady_clock::now();
            _wait_condition_locks.clear();

            async_sleep(SUCCESS_WAIT_DURATION, yield);
        }

        _wait_condition_locks.clear();
    }

    std::shared_ptr<Peer> make_peer(const udp::endpoint& ep)
    {
        auto opt_m = choose_multiplexer_for(*_dht, ep);

        if (!opt_m) {
            LOG_ERROR(_log_path, " Failed to choose multiplexer");
            return nullptr;
        }

        auto utp_client = std::make_unique<UtpOuiServiceClient>
            (_dht->get_executor(), std::move(*opt_m), ep);

        if (!utp_client->verify_remote_endpoint()) {
            LOG_ERROR(_log_path, " Failed to bind uTP client");
            return nullptr;
        }

        if (_connect_proxy) {
            return std::make_unique<ConnectProxyOuiServiceClient>(std::move(utp_client));
        }
        else {
            return utp_client;
        }
    }

    void add_peers(std::set<udp::endpoint> eps)
    {
        auto wan_eps = _dht->wan_endpoints();
        auto lan_eps = _dht->local_endpoints();

        for (auto ep : eps) {
            if (!_dht->is_peer_allowed(ep)) continue;

            // Don't connect to self
            if (wan_eps.count(ep) || lan_eps.count(ep)) continue;

            auto r = _peers.get(ep);
            if (r) continue;  // already known, moved to front
            auto p = make_peer(ep);
            if (!p) continue;
            _peers.put(ep, std::move(p));
        }
    }
};

class Bep5Client::InjectorPinger {
public:
    InjectorPinger( std::shared_ptr<Bep5Client::Swarm> injector_swarm
                  , std::string helper_swarm_name
                  , bool helper_announcement_enabled
                  , std::shared_ptr<bt::DhtBase> dht
                  , Cancel& cancel
                  , const util::LogPath& log_path)
        : _lifetime_cancel(cancel)
        , _injector_swarm(std::move(injector_swarm))
        , _random_generator(std::random_device()())
        , _helper_announcer(std::make_unique<bt::Bep5ManualAnnouncer>( util::sha1_digest(helper_swarm_name)
                                                                     , dht
                                                                     , log_path))
        , _helper_announcement_enabled(helper_announcement_enabled)
    {
        task::spawn_detached(
            _injector_swarm->get_executor(),
            [this, log_path] (asio::yield_context yield) {
                loop(Async(yield, _lifetime_cancel, std::move(log_path)));
            }
        );
    }

    ~InjectorPinger() { _lifetime_cancel(); }

    // Let this pinger known that injector was directly seen from somewhere else so that
    // it can postpone pinging.
    void injector_was_seen_now()
    {
        _injector_was_seen = true;
    }

    bool has_injector_been_seen()
    {
        return _injector_was_seen;
    }
private:
    void loop(Async yield) {
        _injector_swarm->wait_for_ready(yield);

        boost::optional<std::chrono::steady_clock::time_point> _last_ping_time;

        while (true) {
            LOG_DEBUG(yield, " Waiting to ping injectors...");
            _injector_was_seen = false;
            if (_last_ping_time && (Clock::now() - *_last_ping_time) < _ping_frequency) {
                auto d = (*_last_ping_time + _ping_frequency) - Clock::now();
                async_sleep(d, yield);
            }
            LOG_DEBUG(yield, " Waiting to ping injectors: done");

            bool got_reply = _injector_was_seen;

            if (got_reply) {
                // A succesful direct connection during the pause is taken as a sign of reachability.
                if (_helper_announcement_enabled)
                    LOG_DEBUG(yield, " Made connection to injector, announcing as helper (bridge)");
                else
                    LOG_DEBUG(yield, " Made connection to injector, announcements as helper (bridge) are disabled");
            } else {
                got_reply = ping_injectors(select_injectors_to_ping(), yield);

                if (got_reply){
                    if (_helper_announcement_enabled)
                        LOG_DEBUG(yield, " Got pong from injectors, announcing as helper (bridge)");
                    else
                        LOG_DEBUG(yield, " Got pong from injectors, announcements as helper (bridge) are disabled");
                }
                else {
                    LOG_ERROR(yield, " Failed to ping injectors");
                }
            }

            _last_ping_time = Clock::now();

            if (got_reply) {
                if (_helper_announcement_enabled)
                    _helper_announcer->update();
            } else {
                LOG_VERBOSE(yield, " Did not get pong from injectors,"
                                   " the network may be down or they may be blocked");
            }
        }
    }

    [[nodiscard]]
    sys::error_code ping_one_injector(std::shared_ptr<OuiServiceClient> injector, Async yield)
    {
        auto con = injector->connect(yield);
        if (!con.has_value()) return con.error();
        return sys::error_code();
    }

    bool ping_injectors( const std::vector<std::shared_ptr<OuiServiceClient>>& injectors
                       , Async yield)
    {
        WaitCondition wc(get_executor());

        Cancel success_cancel(yield.get_cancel());

        for (auto inj : injectors) {
            yield.spawn(success_cancel, [&, inj, lock = wc.lock()] (Async yield) {
                auto wd = watch_dog(
                        yield.get_executor(),
                        injector_pong_timeout,
                        [&] { yield.cancel(); });

                sys::error_code ec = ping_one_injector(inj, yield);

                if (!ec) {
                    success_cancel();
                }
            });
        }

        wc.wait(yield);

        return bool(success_cancel);
    }

    std::vector<std::shared_ptr<OuiServiceClient>> select_injectors_to_ping() {
        // Select the first (at most) `injectors_to_ping` injectors after shuffling them.
        auto injector_map = _injector_swarm->peers();
        std::vector<std::shared_ptr<OuiServiceClient>> injectors;
        injectors.reserve(injector_map.size());
        for (auto& p : injector_map)
            injectors.push_back(p.second);

        std::shuffle(injectors.begin(), injectors.end(), _random_generator);
        if (injectors.size() > injectors_to_ping)
            injectors.resize(injectors_to_ping);

        return injectors;
    }

    AsioExecutor get_executor() { return _injector_swarm->get_executor(); }

private:
    static const bool _debug = false;  // for development testing only
    Cancel _lifetime_cancel;
    std::shared_ptr<Bep5Client::Swarm> _injector_swarm;
    bool _injector_was_seen = false;
    const Clock::duration _ping_frequency = (_debug ? injector_ping_period_debug : injector_ping_period);
    std::mt19937 _random_generator;
    std::unique_ptr<bt::Bep5ManualAnnouncer> _helper_announcer;
    bool _helper_announcement_enabled = true;
    util::LogPath _log_path;
};

Bep5Client::Bep5Client( std::shared_ptr<bt::DhtBase> dht
                      , std::string injector_swarm_name
                      , asio::ssl::context* injector_tls_ctx
                      , Target targets
                      , const util::LogPath& log_path)
    : _dht(dht)
    , _injector_swarm_name(std::move(injector_swarm_name))
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
    , _default_targets(targets)
{
    if (_dht->local_endpoints().empty()) {
        LOG_ERROR(log_path, " DHT has no endpoints!");
    }
}

Bep5Client::Bep5Client( std::shared_ptr<bt::DhtBase> dht
                      , std::string injector_swarm_name
                      , std::string helpers_swarm_name
                      , bool helper_announcement_enabled
                      , asio::ssl::context* injector_tls_ctx
                      , Target targets
                      , const util::LogPath& log_path)
    : _dht(dht)
    , _injector_swarm_name(std::move(injector_swarm_name))
    , _helpers_swarm_name(std::move(helpers_swarm_name))
    , _helper_announcement_enabled(helper_announcement_enabled)
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
    , _default_targets(targets)
{
    if (_dht->local_endpoints().empty()) {
        LOG_ERROR(log_path, " DHT has no endpoints!");
    }

    assert(_helpers_swarm_name.size());
}

sys::error_code Bep5Client::start(Async yield)
{
    auto slot = _cancel.connect([&] { yield.cancel(); });

    {
        bt::NodeID infohash = util::sha1_digest(_injector_swarm_name);

        LOG_INFO(yield, " Injector swarm: sha1('", _injector_swarm_name, "'): ", infohash.to_hex());

        _injector_swarm.reset(new Swarm(
            infohash,
            _dht,
            injector_swarm_capacity,
            SwarmType::injector,
            _cancel,
            false,
            yield.log_path()
        ));
        _injector_swarm->start();
    }

    if (!_helpers_swarm_name.empty()) {
        bt::NodeID infohash = util::sha1_digest(_helpers_swarm_name);

        LOG_INFO(yield, " Helper swarm (bridges): sha1('", _helpers_swarm_name, "'): ", infohash.to_hex());

        _helpers_swarm.reset(new Swarm(
            infohash,
            _dht,
            helper_swarm_capacity,
            SwarmType::helper,
            _cancel,
            true,
            yield.log_path()
        ));
        _helpers_swarm->start();

        // At least one of the swarm must be ready
        select(
            yield,
            [&](auto yield) { _helpers_swarm->wait_for_ready(yield);  },
            [&](auto yield) { _injector_swarm->wait_for_ready(yield); }
        );

        _injector_pinger.reset(new InjectorPinger(  _injector_swarm
                                                  , _helpers_swarm_name
                                                  , _helper_announcement_enabled
                                                  , _dht
                                                  , _cancel
                                                  , yield.log_path()));
    } else {
        _injector_swarm->wait_for_ready(yield);
    }

    task::spawn_detached(get_executor(), [this] (asio::yield_context yield) {
        status_loop(Async(yield, _cancel));
    });

    return sys::error_code();
}

size_t Bep5Client::injector_candidates_n() const noexcept {
    if (!_injector_swarm){
        return 0;
    }

    return _injector_swarm -> peers().size();
}

void Bep5Client::status_loop(Async yield)
{
    assert(_injector_swarm);
    _injector_swarm->wait_for_ready(yield);

    if (_helpers_swarm) {
        _helpers_swarm->wait_for_ready(yield);
    }

    while (true) {
        async_sleep(1min, yield);

        auto inj_n = _injector_swarm->peers().size();
        auto hlp_n = _helpers_swarm ? _helpers_swarm->peers().size() : 0;

        get_logger().debug(util::str(
            "Bep5Client: Swarm status;",
            " injectors=", inj_n, (inj_n == injector_swarm_capacity ? " (max)" : ""),
            " bridges=", hlp_n, (hlp_n == helper_swarm_capacity ? " (max)" : "")));
    }
}

std::expected<GenericStream, sys::error_code> Bep5Client::connect(Async yield)
{
    return connect(yield, true, _default_targets);
}

using Target = Bep5Client::Target;

struct Bep5Client::Candidates {
    std::set<udp::endpoint> used_candidates;
    boost::optional<udp::endpoint> preferred_ep;
    std::optional<Candidate> preferred;
    std::vector<Candidate> inj_candidates;
    std::vector<Candidate> hlp_candidates;
    std::default_random_engine rand_engine;

    Candidates(boost::optional<udp::endpoint> const& preferred_ep) :
        preferred_ep(preferred_ep),
        rand_engine(std::chrono::system_clock::now().time_since_epoch().count())
    {}

    void try_insert(Candidate candidate) {
        if (used_candidates.count(candidate.endpoint)) {
            return;
        }

        if (preferred_ep && *preferred_ep == candidate.endpoint) {
            preferred = candidate;
            return;
        }

        if (is_in(candidate, inj_candidates)) return;
        if (is_in(candidate, hlp_candidates)) return;

        switch (candidate.swarm_type) {
            case SwarmType::injector:
                inj_candidates.push_back(candidate);
                break;
            case SwarmType::helper:
                hlp_candidates.push_back(candidate);
                break;
        }
    }

    std::optional<Candidate> pick_candidate() {
        std::optional<Candidate> ret;

        if (preferred) {
            ret = std::move(*preferred);
            preferred.reset();
        }
        else {
            ret = random_remove_from(inj_candidates);
            if (!ret) ret = random_remove_from(hlp_candidates);
        }

        if (ret) {
            used_candidates.insert(ret->endpoint);
        }

        return ret;
    }

    std::optional<Candidate> random_remove_from(std::vector<Candidate>& candidates) {
        if (candidates.empty()) return {};
        std::uniform_int_distribution<size_t> random{0, candidates.size() - 1};
        auto i = candidates.begin() + random(rand_engine);
        auto ret = std::move(*i);
        auto last = candidates.end() - 1;
        if (i != last) {
            *i = std::move(*last);
        }
        candidates.resize(candidates.size() - 1);
        return ret;
    }

    bool is_in(Candidate& c, std::vector<Candidate> const& in) const {
        for (auto& i : in) {
            if (c.endpoint == i.endpoint) return true;
        }
        return false;
    }
};

std::expected<GenericStream, sys::error_code>
Bep5Client::connect(Async yield, bool use_tls, Target target)
{
    auto slot = _cancel.connect([&] { yield.cancel(); });

    auto exec = get_executor();

    asio::experimental::channel<void(sys::error_code, std::vector<Candidate>)> channel(exec, 2);

    auto inj_swarm = (target & Target::injectors) ? _injector_swarm.get() : nullptr;
    auto hlp_swarm = (target & Target::helpers)   ? _helpers_swarm. get() : nullptr;

    if (!inj_swarm && !hlp_swarm) {
        return std::unexpected(asio::error::network_unreachable);
    }

    Cancel spawn_cancel(yield.get_cancel()); // Cancels all spawned coroutines

    auto channel_slot = spawn_cancel.connect([&] { if (channel.is_open()) channel.close(); });

    WaitCondition wc(exec);

    size_t job_count = (inj_swarm ? 1 : 0) + (hlp_swarm ? 1 : 0);

    for (auto swarm : std::array<Swarm*, 2>{inj_swarm, hlp_swarm}) {
        if (swarm == nullptr) continue;

        yield.spawn(spawn_cancel, [&job_count, &channel, swarm, lock = wc.lock()] (Async yield) {
            std::vector<Candidate> peers;

            LOG_DEBUG(yield, " Looking up peers in swarm ", *swarm);

            while (true) {
                swarm->wait_for_ready(yield);
                peers = swarm->candidates();

                if (!peers.empty()) {
                    break;
                }
            }

            LOG_DEBUG(yield, " Found ", peers.size(), " peers in swarm ", *swarm, ":");
            for (auto peer: peers) {
                LOG_DEBUG(yield, "     ", peer.endpoint);
            }

            channel.async_send(sys::error_code(), std::move(peers), yield);

            if (--job_count == 0 && channel.is_open()) {
                channel.close();
            }
        });
    }

    struct Result {
        SwarmType swarm_type;
        asio::ip::udp::endpoint endpoint;
        GenericStream connection;
    };

    std::optional<Result> result;

    Candidates candidates(_last_working_ep);

    auto concurrency = std::make_optional<util::Semaphore>(10, exec);
    auto concurrency_slot = spawn_cancel.connect([&concurrency] { concurrency.reset(); });

    while (!spawn_cancel) {
        auto new_candidates = channel.async_receive(yield);
        if (!new_candidates.has_value()) {
            break;
        }

        for (auto& candidate : *new_candidates) {
            candidates.try_insert(candidate);
        }

        while (auto peer = candidates.pick_candidate()) {
            if (!concurrency) break;

            std::optional<util::Semaphore::Lock> concurrency_lock;

            try {
                concurrency_lock = concurrency->await_lock(yield).value();
            }
            catch (Async::Cancelled const&) {
                if (yield.is_cancelled()) throw;
                // Otherwise cancelled by `spawn_cancel`. In which case we just
                // exit the loops.
                break;
            }

            asio::steady_timer timer(exec);
            timer.expires_after(100ms);
            timer.async_wait([cl = std::move(concurrency_lock).value()] (auto) {});

            yield.spawn([
                self = this,
                peer,
                use_tls,
                &spawn_cancel,
                &result,
                lock = wc.lock()
            ] (Async yield) mutable {
                LOG_DEBUG(yield, " Connecting to ", peer->swarm_type, "; ep=", peer->endpoint, "...");
                auto con = self->connect_single(*peer->client, use_tls, yield);
                if (!con) {
                    return;
                }

                result = Result {
                    peer->swarm_type,
                    peer->endpoint,
                    std::move(*con)
                };

                spawn_cancel();
            });
        }
    }

    wc.wait(yield);

    if (!result) {
        LOG_DEBUG(yield, " Did not connect to injector");
        _last_working_ep = boost::none;
        return std::unexpected(asio::error::network_unreachable);
    }

    _last_working_ep = result->endpoint;

    if (result->swarm_type == SwarmType::injector && _injector_pinger) {
        _injector_pinger->injector_was_seen_now();
    }

    LOG_DEBUG(yield, " Connected to ", result->swarm_type, "; ep=", result->endpoint);
    return std::move(result->connection);
}

std::expected<GenericStream, sys::error_code>
Bep5Client::connect_single(OuiServiceClient& cli, bool use_tls, Async yield)
{
    auto con = cli.connect(yield);
    if (!con.has_value()) return std::unexpected(con.error());

    if (!use_tls) return std::move(*con);

    assert(_injector_tls_ctx);

    if (!_injector_tls_ctx) {
        return std::unexpected(asio::error::bad_descriptor);
    }

    return ssl::util::client_handshake(std::move(*con), *_injector_tls_ctx, "" , yield);
}

Bep5Client::~Bep5Client()
{
    _cancel();
    _injector_swarm = nullptr;
    _helpers_swarm  = nullptr;
    _injector_pinger = nullptr;
}

AsioExecutor Bep5Client::get_executor()
{
    return _dht->get_executor();
}
