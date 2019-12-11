#include "client.h"
#include "../utp.h"
#include "../tls.h"
#include "../../async_sleep.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../bittorrent/is_martian.h"
#include "../../logger.h"
#include "../../util/hash.h"

using namespace std;
using namespace ouinet;
using namespace ouiservice;

namespace bt = bittorrent;

using udp = asio::ip::udp;

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

public:
    Bep5Client* owner;

private:
    shared_ptr<bt::MainlineDht> _dht;
    bt::NodeID _infohash;
    Cancel _lifetime_cancel;
    size_t _get_peers_call_count = 0;
    std::vector<WaitCondition::Lock> _wait_condition_locks;
    Peers _peers;
    asio::ssl::context* _tls_ctx;

public:
    Swarm( Bep5Client* owner
         , bt::NodeID infohash
         , shared_ptr<bt::MainlineDht> dht
         , asio::ssl::context* tls_ctx)
        : owner(owner)
        , _dht(move(dht))
        , _infohash(infohash)
        , _tls_ctx(tls_ctx)
    {}

    ~Swarm() {
        _wait_condition_locks.clear();
        _lifetime_cancel();
    }

    void start() {
        asio::spawn(_dht->get_executor()
                   , [&] (asio::yield_context yield) {
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
                LOG_DEBUG("Bep5Client: getting peers from swarm ", _infohash);
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
                LOG_DEBUG("Bep5Client: new endpoints: ", endpoints.size());
                for (auto ep: endpoints) {
                    LOG_DEBUG("Bep5Client:     ", ep);
                }
            }

            add_peers(move(endpoints));

            async_sleep(ex, 1min, cancel, yield);
        }
    }

    bool log_debug() {
        if (!owner) return false;
        return owner->_log_debug;
    }

    shared_ptr<Peer> make_peer(const udp::endpoint& ep)
    {
        auto opt_m = choose_multiplexer_for(*_dht, ep);

        if (!opt_m) {
            LOG_ERROR("Bep5Client: Failed to choose multiplexer");
            return nullptr;
        }

        auto utp_client = make_unique<UtpOuiServiceClient>
            (_dht->get_executor(), move(*opt_m), util::str(ep));

        if (!utp_client->verify_remote_endpoint()) {
            LOG_ERROR("Bep5Client: Failed to bind uTP client");
            return nullptr;
        }

        if (!_tls_ctx) {
            return utp_client;
        }

        auto tls_client = make_shared<TlsOuiServiceClient>(move(utp_client), *_tls_ctx);

        return tls_client;
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

Bep5Client::Bep5Client( shared_ptr<bt::MainlineDht> dht
                      , string injector_swarm_name
                      , asio::ssl::context* injector_tls_ctx)
    : _dht(dht)
    , _injector_swarm_name(move(injector_swarm_name))
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
{
    if (_dht->local_endpoints().empty()) {
        LOG_ERROR("Bep5Client: DHT has no endpoints!");
    }
}

Bep5Client::Bep5Client( shared_ptr<bt::MainlineDht> dht
                      , string injector_swarm_name
                      , string helpers_swarm_name
                      , asio::ssl::context* injector_tls_ctx)
    : _dht(dht)
    , _injector_swarm_name(move(injector_swarm_name))
    , _helpers_swarm_name(move(helpers_swarm_name))
    , _injector_tls_ctx(injector_tls_ctx)
    , _random_generator(std::random_device()())
{
    if (_dht->local_endpoints().empty()) {
        LOG_ERROR("Bep5Client: DHT has no endpoints!");
    }

    assert(_helpers_swarm_name.size());

    if (_helpers_swarm_name.size()) {
        bt::NodeID infohash = util::sha1_digest(_helpers_swarm_name);
        _helper_announcer = make_unique<bt::Bep5PeriodicAnnouncer>(infohash, _dht);
    }
}

void Bep5Client::start(asio::yield_context)
{
    {
        bt::NodeID infohash = util::sha1_digest(_injector_swarm_name);

        LOG_INFO("Injector swarm: sha1('", _injector_swarm_name, "'): ", infohash.to_hex());

        _injector_swarm.reset(new Swarm(this, infohash, _dht, _injector_tls_ctx));
        _injector_swarm->start();
    }

    if (!_helpers_swarm_name.empty()) {
        bt::NodeID infohash = util::sha1_digest(_helpers_swarm_name);

        LOG_INFO("Helpers swarm: sha1('", _helpers_swarm_name, "'): ", infohash.to_hex());

        _helpers_swarm.reset(new Swarm(this, infohash, _dht, nullptr));
        _helpers_swarm->start();
    }
}

void Bep5Client::stop()
{
    _cancel();
    _injector_swarm = nullptr;
    _helpers_swarm  = nullptr;
}

std::vector<Bep5Client::Candidate> Bep5Client::get_peers()
{
    std::vector<Candidate> inj;
    std::vector<Candidate> hlp;

    auto& inj_m = _injector_swarm->peers();
    auto* hlp_m = _helpers_swarm ? &_helpers_swarm->peers() : nullptr;

    inj.reserve(inj_m.size());
    for (auto p : inj_m) inj.push_back({p.first, p.second});

    if (hlp_m) {
        hlp.reserve(hlp_m->size());
        for (auto p : *hlp_m) hlp.push_back({p.first, p.second});
    }

    std::shuffle(inj.begin(), inj.end(), _random_generator);
    std::shuffle(hlp.begin(), hlp.end(), _random_generator);

    std::vector<Candidate> ret;
    ret.reserve(inj.size() + hlp.size());

    for (auto& p : inj) { ret.push_back(p); }
    for (auto& p : hlp) { ret.push_back(p); }

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

GenericStream Bep5Client::connect(asio::yield_context yield, Cancel& cancel_)
{
    assert(!_cancel);
    assert(!cancel_);

    Cancel cancel(cancel_);
    auto cancel_con = _cancel.connect([&] { cancel(); });

    sys::error_code ec;

    _injector_swarm->wait_for_ready(cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, GenericStream{});

    if (_helpers_swarm) {
        _helpers_swarm->wait_for_ready(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, GenericStream{});
    }

    WaitCondition wc(get_executor());

    Cancel spawn_cancel(cancel); // Cancels all spawned coroutines

    asio::ip::udp::endpoint ret_ep;
    GenericStream ret_con;

    uint32_t i = 0;

    for (auto peer : get_peers()) {
        auto j = i++;

        const uint32_t k = 10;
        uint32_t delay_ms = (j <= k) ? 0 : ((j-k) * 100);

        asio::spawn(get_executor(),
        [ =
        , ex = get_executor()
        , &spawn_cancel
        , &ret_con
        , &ret_ep
        , lock = wc.lock()
        ] (asio::yield_context y) mutable {
            sys::error_code ec;

            if (delay_ms) {
                async_sleep(ex, chrono::milliseconds(delay_ms), spawn_cancel, yield);
                if (spawn_cancel) return;
            }

            auto con = peer.client->connect(y[ec], spawn_cancel);
            assert(!spawn_cancel || ec == asio::error::operation_aborted);
            if (spawn_cancel || ec) return;
            ret_con = move(con);
            ret_ep  = peer.endpoint;
            spawn_cancel();
        });
    }

    wc.wait(cancel, yield[ec]);

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
    } else {
        _last_working_ep = ret_ep;
    }

    return or_throw(yield, ec, move(ret_con));
}

Bep5Client::~Bep5Client()
{
    stop();
}

asio::executor Bep5Client::get_executor()
{
    return _dht->get_executor();
}
