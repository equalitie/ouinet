#include <boost/functional/hash.hpp>

#include "client.h"
#include "../utp.h"
#include "../connect_proxy.h"
#include "../tls.h"
#include "../../async_sleep.h"
#include "../../bittorrent/mainline_dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../bittorrent/is_martian.h"
#include "../../logger.h"
#include "../../util/hash.h"
#include "../../util/lru_cache.h"
#include "../../ssl/util.h"
#include "../../util/handler_tracker.h"
#include "../../util/wait_condition.h"
#include "../../util/watch_dog.h"
#include "../../util/semaphore.h"
#include <boost/asio/experimental/channel.hpp>

#define _LOGPFX "Bep5Client: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _VERBOSE(...)  LOG_VERBOSE(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

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

using namespace std;
using namespace ouinet;
using namespace ouiservice;

namespace bt = bittorrent;

using udp = asio::ip::udp;
using Clock = chrono::steady_clock;

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

constexpr chrono::duration ERROR_WAIT_DURATION = 1s;
constexpr chrono::duration SUCCESS_WAIT_DURATION = 1min;

struct Bep5Client::Swarm
{
private:
    using Peer  = AbstractClient;
    using Peers = util::LruCache<asio::ip::udp::endpoint, std::shared_ptr<Peer>>;

private:
    Bep5Client* _owner;
    shared_ptr<bt::DhtBase> _dht;
    bt::NodeID _infohash;
    SwarmType _type;
    Cancel _lifetime_cancel;
    std::optional<chrono::steady_clock::time_point> _last_success_time;
    std::vector<WaitCondition::Lock> _wait_condition_locks;
    Peers _peers;
    const bool _connect_proxy;

public:
    Swarm( Bep5Client* owner
         , bt::NodeID infohash
         , shared_ptr<bt::DhtBase> dht
         , size_t capacity
         , SwarmType type
         , Cancel& cancel
         , bool connect_proxy)
        : _owner(owner)
        , _dht(move(dht))
        , _infohash(infohash)
        , _type(type)
        , _lifetime_cancel(cancel)
        , _peers(capacity)
        , _connect_proxy(connect_proxy)
    {}

    ~Swarm() {
        _wait_condition_locks.clear();
        _lifetime_cancel();
    }

    void start() {
        TRACK_SPAWN(_dht->get_executor(), [&] (asio::yield_context yield) {
            Cancel cancel(_lifetime_cancel);
            sys::error_code ec;
            loop(cancel, yield[ec]);
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
        auto duration = chrono::steady_clock::now() - *_last_success_time;
        return duration < 5 * SUCCESS_WAIT_DURATION;
    }

    void wait_for_ready(Cancel cancel, asio::yield_context yield) {
        if (is_ready()) return;

        WaitCondition wc(_dht->get_executor());

        _wait_condition_locks.push_back(wc.lock());

        sys::error_code ec;
        wc.wait(cancel, yield[ec]);

        if (cancel)
            return or_throw(yield, asio::error::operation_aborted);
    }

    AsioExecutor get_executor() { return _dht->get_executor(); }

private:
    void loop(Cancel& cancel, asio::yield_context yield) {
        auto ex = _dht->get_executor();

        {
            sys::error_code ec;
            _dht->wait_all_ready(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec);
        }

        while (!cancel) {
            sys::error_code ec;

            if (log_debug()) {
                _DEBUG("Getting peers from swarm ", _infohash);
            }

            auto endpoints = _dht->tracker_get_peers(_infohash, cancel, yield[ec]);

            assert(!cancel || ec == asio::error::operation_aborted);

            if (cancel) break;

            if (ec) {
                async_sleep(ERROR_WAIT_DURATION, cancel, yield);
                continue;
            }

            _last_success_time = chrono::steady_clock::now();

            if (log_debug()) {
                _DEBUG("New endpoints: ", endpoints.size());
                for (auto ep: endpoints) {
                    _DEBUG("    ", ep);
                }
            }

            add_peers(move(endpoints));
            _wait_condition_locks.clear();

            async_sleep(SUCCESS_WAIT_DURATION, cancel, yield);
        }

        _wait_condition_locks.clear();
    }

    bool log_debug() {
        if (!_owner) return false;
        return _owner->_log_debug;
    }

    shared_ptr<Peer> make_peer(const udp::endpoint& ep)
    {
        auto opt_m = choose_multiplexer_for(*_dht, ep);

        if (!opt_m) {
            _ERROR("Failed to choose multiplexer");
            return nullptr;
        }

        auto utp_client = make_unique<UtpOuiServiceClient>
            (_dht->get_executor(), move(*opt_m), ep);

        if (!utp_client->verify_remote_endpoint()) {
            _ERROR("Failed to bind uTP client");
            return nullptr;
        }

        if (_connect_proxy) {
            return make_unique<ConnectProxyOuiServiceClient>(move(utp_client));
        }
        else {
            return utp_client;
        }
    }

    void add_peers(set<udp::endpoint> eps)
    {
        auto wan_eps = _dht->wan_endpoints();
        auto lan_eps = _dht->local_endpoints();

        for (auto ep : eps) {
            if (_dht->is_martian(ep)) continue;

            // Don't connect to self
            if (wan_eps.count(ep) || lan_eps.count(ep)) continue;

            auto r = _peers.get(ep);
            if (r) continue;  // already known, moved to front
            auto p = make_peer(ep);
            if (!p) continue;
            _peers.put(ep, move(p));
        }
    }
};

class Bep5Client::InjectorPinger {
public:
    InjectorPinger( shared_ptr<Bep5Client::Swarm> injector_swarm
                  , string helper_swarm_name
                  , bool helper_announcement_enabled
                  , shared_ptr<bt::DhtBase> dht
                  , Cancel& cancel)
        : _lifetime_cancel(cancel)
        , _injector_swarm(move(injector_swarm))
        , _random_generator(std::random_device()())
        , _helper_announcer(new bt::Bep5ManualAnnouncer(util::sha1_digest(helper_swarm_name), dht))
        , _helper_announcement_enabled(helper_announcement_enabled)
    {
        TRACK_SPAWN(_injector_swarm->get_executor(),
                    [this] (asio::yield_context yield) {
            sys::error_code ec;
            loop(yield[ec]);
        });
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
    void loop(asio::yield_context yield) {
        Cancel cancel(_lifetime_cancel);

        sys::error_code ec;
        _injector_swarm->wait_for_ready(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        boost::optional<chrono::steady_clock::time_point> _last_ping_time;
        while (!cancel) {
            _DEBUG("Waiting to ping injectors...");
            _injector_was_seen = false;
            if (_last_ping_time && (Clock::now() - *_last_ping_time) < _ping_frequency) {
                auto d = (*_last_ping_time + _ping_frequency) - Clock::now();
                async_sleep(d, cancel, yield);
                if (cancel) return;
            }
            _DEBUG("Waiting to ping injectors: done");

            bool got_reply = _injector_was_seen;
            if (got_reply) {
                // A succesful direct connection during the pause is taken as a sign of reachability.
                if (_helper_announcement_enabled)
                    _DEBUG("Made connection to injector, announcing as helper (bridge)");
                else
                    _DEBUG("Made connection to injector, announcements as helper (bridge) are disabled");
            } else {
                got_reply = ping_injectors(select_injectors_to_ping(), cancel, yield[ec]);
                if (!cancel && ec)
                    _ERROR("Failed to ping injectors; ec=", ec);
                return_or_throw_on_error(yield, cancel, ec);
                if (got_reply){
                    if (_helper_announcement_enabled)
                        _DEBUG("Got pong from injectors, announcing as helper (bridge)");
                    else
                        _DEBUG("Got pong from injectors, announcements as helper (bridge) are disabled");
                }
            }

            _last_ping_time = Clock::now();

            if (got_reply) {
                if (_helper_announcement_enabled)
                    _helper_announcer->update();
            } else {
                _VERBOSE("Did not get pong from injectors,"
                         " the network may be down or they may be blocked");
            }
        }
    }

    bool ping_one_injector( shared_ptr<AbstractClient> injector
                          , Cancel& cancel
                          , asio::yield_context yield)
    {
        sys::error_code ec;
        auto con = injector->connect(yield[ec], cancel);
        return_or_throw_on_error(yield, cancel, ec, false);
        return true;
    }

    bool ping_injectors( const std::vector<shared_ptr<AbstractClient>>& injectors
                       , Cancel cancel
                       , asio::yield_context yield)
    {
        auto ex = get_executor();

        WaitCondition wc(ex);

        Cancel success_cancel(cancel);

        for (auto inj : injectors) {
            TRACK_SPAWN(ex, ([&, inj, lock = wc.lock()]
                    (asio::yield_context yield) {
                Cancel c(cancel);
                auto sc = success_cancel.connect([&] { c(); });

                sys::error_code ec;
                auto wd = watch_dog(ex, injector_pong_timeout, [&] { c(); });
                if (ping_one_injector(inj, c, yield[ec])) {
                    success_cancel();
                }
            }));
        }

        sys::error_code ec;
        wc.wait(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, false);

        return bool(success_cancel);
    }

    std::vector<shared_ptr<AbstractClient>> select_injectors_to_ping() {
        // Select the first (at most) `injectors_to_ping` injectors after shuffling them.
        auto injector_map = _injector_swarm->peers();
        std::vector<shared_ptr<AbstractClient>> injectors;
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
    shared_ptr<Bep5Client::Swarm> _injector_swarm;
    bool _injector_was_seen = false;
    const Clock::duration _ping_frequency = (_debug ? injector_ping_period_debug : injector_ping_period);
    std::mt19937 _random_generator;
    std::unique_ptr<bt::Bep5ManualAnnouncer> _helper_announcer;
    bool _helper_announcement_enabled = true;
};

Bep5Client::Bep5Client( shared_ptr<bt::DhtBase> dht
                      , string injector_swarm_name
                      , asio::ssl::context* injector_tls_ctx
                      , Target targets)
    : _dht(dht)
    , _injector_swarm_name(move(injector_swarm_name))
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
    , _default_targets(targets)
{
    if (_dht->local_endpoints().empty()) {
        _ERROR("DHT has no endpoints!");
    }
}

Bep5Client::Bep5Client( shared_ptr<bt::DhtBase> dht
                      , string injector_swarm_name
                      , string helpers_swarm_name
                      , bool helper_announcement_enabled
                      , asio::ssl::context* injector_tls_ctx
                      , Target targets)
    : _dht(dht)
    , _injector_swarm_name(move(injector_swarm_name))
    , _helpers_swarm_name(move(helpers_swarm_name))
    , _helper_announcement_enabled(helper_announcement_enabled)
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
    , _default_targets(targets)
{
    if (_dht->local_endpoints().empty()) {
        _ERROR("DHT has no endpoints!");
    }

    assert(_helpers_swarm_name.size());
}

void Bep5Client::start(asio::yield_context yield)
{
    {
        bt::NodeID infohash = util::sha1_digest(_injector_swarm_name);

        _INFO("Injector swarm: sha1('", _injector_swarm_name, "'): ", infohash.to_hex());

        _injector_swarm.reset(new Swarm(this, infohash, _dht, injector_swarm_capacity, SwarmType::injector, _cancel, false));
        _injector_swarm->start();
    }

    if (!_helpers_swarm_name.empty()) {
        bt::NodeID infohash = util::sha1_digest(_helpers_swarm_name);

        _INFO("Helper swarm (bridges): sha1('", _helpers_swarm_name, "'): ", infohash.to_hex());

        _helpers_swarm.reset(new Swarm(this, infohash, _dht, helper_swarm_capacity, SwarmType::helper, _cancel, true));
        _helpers_swarm->start();

        _helpers_swarm->wait_for_ready(_cancel, yield);
        if (_cancel) return;
        _injector_swarm->wait_for_ready(_cancel, yield);
        if (_cancel) return;

        _injector_pinger.reset(new InjectorPinger(  _injector_swarm
                                                  , _helpers_swarm_name
                                                  , _helper_announcement_enabled
                                                  , _dht
                                                  , _cancel));
    }

    TRACK_SPAWN(get_executor(),
                [this] (asio::yield_context yield) {
        sys::error_code ec;
        status_loop(yield[ec]);
    });
}

size_t Bep5Client::injector_candidates_n() const noexcept {
    if (!_injector_swarm){
        return 0;
    }

    return _injector_swarm -> peers().size();
}

void Bep5Client::stop()
{
    _cancel();
    _injector_swarm = nullptr;
    _helpers_swarm  = nullptr;
    _injector_pinger = nullptr;
}

void Bep5Client::status_loop(asio::yield_context yield)
{
    assert(!_cancel);

    Cancel cancel(_cancel);
    sys::error_code ec;

    assert(_injector_swarm);
    {
        _injector_swarm->wait_for_ready(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }

    if (_helpers_swarm) {
        _helpers_swarm->wait_for_ready(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }

    while (!cancel) {
        ec = {};
        async_sleep(1min, cancel, yield[ec]);

        if (ec || cancel || logger.get_threshold() > DEBUG)
            continue;

        auto inj_n = _injector_swarm->peers().size();
        auto hlp_n = _helpers_swarm ? _helpers_swarm->peers().size() : 0;
        logger.debug(util::str(
            "Bep5Client: Swarm status;",
            " injectors=", inj_n, (inj_n == injector_swarm_capacity ? " (max)" : ""),
            " bridges=", hlp_n, (hlp_n == helper_swarm_capacity ? " (max)" : "")));
    }
}

GenericStream Bep5Client::connect(asio::yield_context yield, Cancel& cancel)
{
    return connect(yield, cancel, true, _default_targets);
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
        rand_engine(chrono::system_clock::now().time_since_epoch().count())
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

GenericStream Bep5Client::connect( asio::yield_context yield
                                 , Cancel& cancel_
                                 , bool use_tls
                                 , Target target)
{
    assert(!_cancel);
    assert(!cancel_);

    Cancel cancel(cancel_);
    auto cancel_con = _cancel.connect([&] { cancel(); });

    auto exec = get_executor();

    asio::experimental::channel<void(sys::error_code, std::vector<Candidate>)> channel(exec, 2);

    auto inj_swarm = (target & Target::injectors) ? _injector_swarm.get() : nullptr;
    auto hlp_swarm = (target & Target::helpers)   ? _helpers_swarm. get() : nullptr;

    if (!inj_swarm && !hlp_swarm) {
        return or_throw<GenericStream>(yield, asio::error::network_unreachable);
    }

    Cancel spawn_cancel(cancel); // Cancels all spawned coroutines

    auto close_channel_con = spawn_cancel.connect([&] { if (channel.is_open()) channel.close(); });

    WaitCondition wc(exec);

    size_t job_count = (inj_swarm ? 1 : 0) + (hlp_swarm ? 1 : 0);

    for (auto swarm : std::array<Swarm*, 2>{inj_swarm, hlp_swarm}) {
        if (swarm == nullptr) continue;

        TRACK_SPAWN(exec, ([&job_count, &channel, &spawn_cancel, swarm, lock = wc.lock()] (auto yield) {
            sys::error_code ec;
            swarm->wait_for_ready(spawn_cancel, yield[ec]);
            if (!ec) {
                auto peers = swarm->candidates();
                channel.async_send(sys::error_code(), std::move(peers), yield[ec]);
            }
            if (--job_count == 0 && channel.is_open()) {
                channel.close();
            }
        }));
    }

    struct Result {
        SwarmType swarm_type;
        asio::ip::udp::endpoint endpoint;
        GenericStream connection;
    };

    std::optional<Result> result;

    Candidates candidates(_last_working_ep);

    auto concurrency = std::make_optional<util::Semaphore>(10, exec);
    auto reset_concurrency = spawn_cancel.connect([&concurrency] { concurrency.reset(); });

    while (true) {
        sys::error_code channel_ec;
        auto new_candidates = channel.async_receive(yield[channel_ec]);

        if (cancel) channel_ec = asio::error::operation_aborted;
        if (channel_ec) break;

        for (auto& candidate : new_candidates) {
            candidates.try_insert(candidate);
        }

        while (auto peer = candidates.pick_candidate()) {
            sys::error_code concurrency_ec;
            if (!concurrency) break;
            auto concurrency_lock = concurrency->await_lock(yield[concurrency_ec]);
            if (concurrency_ec || spawn_cancel) break;

            asio::steady_timer timer(exec);
            timer.expires_after(100ms);
            timer.async_wait([cl = std::move(concurrency_lock)] (auto) {});

            TRACK_SPAWN(exec, ([
                self = this,
                peer,
                use_tls,
                &spawn_cancel,
                &result,
                lock = wc.lock()
            ] (asio::yield_context y) mutable {
                _DEBUG("trying to contact", peer->endpoint);

                sys::error_code ec;

                auto con = self->connect_single(*peer->client, use_tls, spawn_cancel, y[ec]);

                assert(!spawn_cancel || ec == asio::error::operation_aborted);
                if (spawn_cancel || ec) return;

                result = Result {
                    peer->swarm_type,
                    peer->endpoint,
                    std::move(con)
                };
                spawn_cancel();
            }));
        }
    }

    sys::error_code ec;
    wc.wait(yield[ec]);

    if (cancel) {
        ec = asio::error::operation_aborted;
    }
    else if (!result) {
        ec = asio::error::network_unreachable;
    }
    else {
        assert(!ec);
        ec = {};
    }

    if (ec) {
        _last_working_ep = boost::none;

        _DEBUG("Did not connect to injector; ec=", ec);
        return or_throw<GenericStream>(yield, ec);
    } else {
        _last_working_ep = result->endpoint;

        if (result->swarm_type == SwarmType::injector && _injector_pinger) {
            _injector_pinger->injector_was_seen_now();
        }

        _DEBUG("Connected to ", result->swarm_type, "; ep=", result->endpoint);
        return or_throw(yield, ec, std::move(result->connection));
    }
}

GenericStream
Bep5Client::connect_single( AbstractClient& cli
                          , bool use_tls
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    sys::error_code ec;
    auto con = cli.connect(yield[ec], cancel);
    return_or_throw_on_error(yield, cancel, ec, GenericStream{});

    if (!use_tls) return con;

    assert(_injector_tls_ctx);

    if (!_injector_tls_ctx) {
        return or_throw<GenericStream>(yield, asio::error::bad_descriptor);
    }

    return ssl::util::client_handshake( std::move(con)
                                      , *_injector_tls_ctx, ""
                                      , cancel
                                      , yield);
}

Bep5Client::~Bep5Client()
{
    stop();
}

AsioExecutor Bep5Client::get_executor()
{
    return _dht->get_executor();
}
