#include "client.h"
#include "../utp.h"
#include "../connect_proxy.h"
#include "../tls.h"
#include "../../async_sleep.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../bittorrent/is_martian.h"
#include "../../logger.h"
#include "../../util/hash.h"
#include "../../ssl/util.h"
#include "../../util/handler_tracker.h"

#define _LOGPFX "Bep5Client: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _VERBOSE(...)  LOG_VERBOSE(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

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
choose_multiplexer_for(bt::MainlineDht& dht, const udp::endpoint& ep)
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

struct Bep5Client::Swarm
{
private:
    using Peer  = AbstractClient;
    using Peers = std::map<asio::ip::udp::endpoint, std::shared_ptr<Peer>>;

private:
    Bep5Client* _owner;
    shared_ptr<bt::MainlineDht> _dht;
    bt::NodeID _infohash;
    Cancel _lifetime_cancel;
    size_t _get_peers_call_count = 0;
    std::vector<WaitCondition::Lock> _wait_condition_locks;
    Peers _peers;
    const bool _connect_proxy;

public:
    Swarm( Bep5Client* owner
         , bt::NodeID infohash
         , shared_ptr<bt::MainlineDht> dht
         , Cancel& cancel
         , bool connect_proxy)
        : _owner(owner)
        , _dht(move(dht))
        , _infohash(infohash)
        , _lifetime_cancel(cancel)
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

    void wait_for_ready(Cancel cancel, asio::yield_context yield) {
        if (_get_peers_call_count != 0) return;

        WaitCondition wc(_dht->get_executor());

        _wait_condition_locks.push_back(wc.lock());

        sys::error_code ec;
        wc.wait(cancel, yield[ec]);

        if (cancel)
            return or_throw(yield, asio::error::operation_aborted);
    }

    asio::executor get_executor() { return _dht->get_executor(); }

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

            _get_peers_call_count++;
            _wait_condition_locks.clear();

            if (ec) {
                async_sleep(ex, 1s, cancel, yield);
                continue;
            }

            if (log_debug()) {
                _DEBUG("New endpoints: ", endpoints.size());
                for (auto ep: endpoints) {
                    _DEBUG("    ", ep);
                }
            }

            add_peers(move(endpoints));

            async_sleep(ex, 1min, cancel, yield);
        }
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
            (_dht->get_executor(), move(*opt_m), util::str(ep));

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
            if (bittorrent::is_martian(ep)) continue;

            // Don't connect to self
            if (wan_eps.count(ep) || lan_eps.count(ep)) continue;

            auto r = _peers.emplace(ep, nullptr);

            if (r.second) {
                auto p = make_peer(ep);
                if (!p) continue;
                r.first->second = move(p);
            }
        }
    }
};

class Bep5Client::InjectorPinger {
public:
    InjectorPinger( shared_ptr<Bep5Client::Swarm> injector_swarm
                  , string helper_swarm_name
                  , shared_ptr<bt::MainlineDht> dht
                  , Cancel& cancel)
        : _lifetime_cancel(cancel)
        , _injector_swarm(move(injector_swarm))
        , _random_generator(std::random_device()())
        , _helper_announcer(new bt::Bep5ManualAnnouncer(util::sha1_digest(helper_swarm_name), dht))
    {
        TRACK_SPAWN(_injector_swarm->get_executor(),
                    [=] (asio::yield_context yield) {
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
                async_sleep(get_executor(), d, cancel, yield);
                if (cancel) return;
            }
            _DEBUG("Waiting to ping injectors: done");

            bool got_reply = _injector_was_seen;
            if (got_reply)
                // A succesful direct connection during the pause is taken as a sign of reachability.
                _DEBUG("Made connection to injector, announcing as helper (bridge)");
            else {
                got_reply = ping_injectors(select_injectors_to_ping(), cancel, yield[ec]);
                if (!cancel && ec)
                    _ERROR("Failed to ping injectors ec:", ec.message());
                return_or_throw_on_error(yield, cancel, ec);
                if (got_reply)
                    _DEBUG("Got pong from injectors, announcing as helper (bridge)");
            }

            _last_ping_time = Clock::now();

            if (got_reply)
                _helper_announcer->update();
            else
                _VERBOSE("Did not get pong from injectors,"
                         " the network may be down or they may be blocked");
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
                WatchDog wd(ex, chrono::seconds(60), [&] { c(); });
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
        // Select the first (at most) `max` injectors after shuffling them.
        static const unsigned max = 30;

        auto injector_map = _injector_swarm->peers();
        std::vector<shared_ptr<AbstractClient>> injectors;
        injectors.reserve(injector_map.size());
        for (auto& p : injector_map)
            injectors.push_back(p.second);

        std::shuffle(injectors.begin(), injectors.end(), _random_generator);
        if (injectors.size() > max)
            injectors.resize(max);

        return injectors;
    }

    asio::executor get_executor() { return _injector_swarm->get_executor(); }

private:
    static const bool _debug = false;  // for development testing only
    Cancel _lifetime_cancel;
    shared_ptr<Bep5Client::Swarm> _injector_swarm;
    bool _injector_was_seen = false;
    const Clock::duration _ping_frequency = chrono::minutes(_debug ? 2 : 10);
    std::mt19937 _random_generator;
    std::unique_ptr<bt::Bep5ManualAnnouncer> _helper_announcer;
};

Bep5Client::Bep5Client( shared_ptr<bt::MainlineDht> dht
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

Bep5Client::Bep5Client( shared_ptr<bt::MainlineDht> dht
                      , string injector_swarm_name
                      , string helpers_swarm_name
                      , asio::ssl::context* injector_tls_ctx
                      , Target targets)
    : _dht(dht)
    , _injector_swarm_name(move(injector_swarm_name))
    , _helpers_swarm_name(move(helpers_swarm_name))
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
    , _default_targets(targets)
{
    if (_dht->local_endpoints().empty()) {
        _ERROR("DHT has no endpoints!");
    }

    assert(_helpers_swarm_name.size());
}

void Bep5Client::start(asio::yield_context)
{
    {
        bt::NodeID infohash = util::sha1_digest(_injector_swarm_name);

        _INFO("Injector swarm: sha1('", _injector_swarm_name, "'): ", infohash.to_hex());

        _injector_swarm.reset(new Swarm(this, infohash, _dht, _cancel, false));
        _injector_swarm->start();
    }

    if (!_helpers_swarm_name.empty()) {
        bt::NodeID infohash = util::sha1_digest(_helpers_swarm_name);

        _INFO("Helper swarm (bridges): sha1('", _helpers_swarm_name, "'): ", infohash.to_hex());

        _helpers_swarm.reset(new Swarm(this, infohash, _dht, _cancel, true));
        _helpers_swarm->start();

        _injector_pinger.reset(new InjectorPinger(_injector_swarm, _helpers_swarm_name, _dht, _cancel));
    }

    TRACK_SPAWN(get_executor(),
                [=] (asio::yield_context yield) {
        sys::error_code ec;
        status_loop(yield[ec]);
    });
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
        async_sleep(get_executor(), 1min, cancel, yield[ec]);

        if (ec || cancel || logger.get_threshold() > DEBUG)
            continue;

        auto inj_n = _injector_swarm->peers().size();
        auto hlp_n = _helpers_swarm ? _helpers_swarm->peers().size() : 0;
        logger.debug(util::str(
            "Bep5Client: swarm status:",
            " injectors=", inj_n, " bridges=", hlp_n));
    }
}

std::vector<Bep5Client::Candidate> Bep5Client::get_peers(Target target)
{
    std::vector<Candidate> inj;
    std::vector<Candidate> hlp;

    auto& inj_m = _injector_swarm->peers();
    auto* hlp_m = _helpers_swarm ? &_helpers_swarm->peers() : nullptr;

    if (target & Target::injectors) {
        inj.reserve(inj_m.size());
        for (auto p : inj_m) inj.push_back({p.first, p.second, Target::injectors});
    }

    if (hlp_m && (target & Target::helpers)) {
        hlp.reserve(hlp_m->size());
        for (auto p : *hlp_m) hlp.push_back({p.first, p.second, Target::helpers});
    }

    std::shuffle(inj.begin(), inj.end(), _random_generator);
    std::shuffle(hlp.begin(), hlp.end(), _random_generator);

    std::vector<Candidate> ret;
    ret.reserve(inj.size() + hlp.size());

    for (auto& p : inj) { ret.push_back(p); }
    for (auto& p : hlp) { ret.push_back(p); }

    //auto wan_eps = _dht->wan_endpoints();
    //auto lan_eps = _dht->local_endpoints();
    //cerr << "wan: "; for (auto& i : wan_eps) cerr << i << ","; cerr << "\n";
    //cerr << "lan: "; for (auto& i : lan_eps) cerr << i << ","; cerr << "\n";
    //cerr << "inj: "; for (auto& i : inj) cerr << i.endpoint << ","; cerr << "\n";
    //cerr << "hlp: "; for (auto& i : hlp) cerr << i.endpoint << ","; cerr << "\n";

    // If there is a peer that has woked recently then use that one
    if (_last_working_ep) {
        for (auto i = ret.begin(); i != ret.end(); ++i) {
            if (i->endpoint == *_last_working_ep) {
                if (i != ret.begin()) {
                    std::swap(*i, ret.front());
                }
                break;
            }
        }
    }

    return ret;
}

GenericStream Bep5Client::connect(asio::yield_context yield, Cancel& cancel)
{
    return connect(yield, cancel, true, _default_targets);
}

GenericStream Bep5Client::connect( asio::yield_context yield
                                 , Cancel& cancel_
                                 , bool tls
                                 , Target target)
{
    assert(!_cancel);
    assert(!cancel_);

    Cancel cancel(cancel_);
    auto cancel_con = _cancel.connect([&] { cancel(); });

    sys::error_code ec;

    if (_injector_swarm && (target & Target::injectors)) {
        _injector_swarm->wait_for_ready(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, GenericStream());
    }

    if (_helpers_swarm && (target & Target::helpers)) {
        _helpers_swarm->wait_for_ready(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, GenericStream());
    }

    WaitCondition wc(get_executor());

    Cancel spawn_cancel(cancel); // Cancels all spawned coroutines

    Target ret_target = Target::none;
    asio::ip::udp::endpoint ret_ep;
    GenericStream ret_con;

    uint32_t i = 0;

    auto exec = get_executor();

    for (auto peer : get_peers(target)) {
        auto j = i++;

        const uint32_t k = 10;
        uint32_t delay_ms = (j <= k) ? 0 : ((j-k) * 100);

        TRACK_SPAWN(exec, ([
            =,
            &spawn_cancel,
            &ret_target,
            &ret_con,
            &ret_ep,
            lock = wc.lock()
        ] (asio::yield_context y) mutable {
            sys::error_code ec;

            if (delay_ms) {
                async_sleep(exec, chrono::milliseconds(delay_ms), spawn_cancel, y);
                if (spawn_cancel) return;
            }

            auto con = connect_single(*peer.client, tls, spawn_cancel, y[ec]);
            assert(!spawn_cancel || ec == asio::error::operation_aborted);
            if (spawn_cancel || ec) return;
            ret_target = peer.target;
            ret_con = move(con);
            ret_ep  = peer.endpoint;
            spawn_cancel();
        }));
    }

    wc.wait(yield[ec]);

    if (cancel) {
        ec = asio::error::operation_aborted;
    }
    else if (!ret_con.has_implementation()) {
        ec = asio::error::network_unreachable;
    }
    else {
        assert(!ec);
        ec = {};
    }

    if (ec) {
        _last_working_ep = boost::none;
        _DEBUG( "Did not connect to any peer;"
              , " peers:", i
              , " ec:", ec.message());
    } else {
        _last_working_ep = ret_ep;
        if (ret_target == Target::injectors) {
            if (_injector_pinger)
                _injector_pinger->injector_was_seen_now();
            _DEBUG("Connected to injector peer directly; rep:", ret_ep);
        } else if (ret_target == Target::helpers)
            _DEBUG("Connected to injector via helper peer (bridge); rep:", ret_ep);
        else
            assert(0 && "Invalid peer type");
    }

    return or_throw(yield, ec, move(ret_con));
}

GenericStream
Bep5Client::connect_single( AbstractClient& cli
                          , bool tls
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    sys::error_code ec;
    auto con = cli.connect(yield[ec], cancel);
    return_or_throw_on_error(yield, cancel, ec, GenericStream{});

    if (!tls) return con;

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

asio::executor Bep5Client::get_executor()
{
    return _dht->get_executor();
}
