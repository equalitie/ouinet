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

using AbstractClient = OuiServiceImplementationClient;
using udp = asio::ip::udp;

struct Bep5Client::Bep5Loop
{
    Bep5Client* owner;
    shared_ptr<bt::MainlineDht> dht;
    bt::NodeID infohash;
    Cancel cancel_;
    size_t get_peers_call_count = 0;
    std::vector<WaitCondition::Lock> wait_condition_locks;

    Bep5Loop( Bep5Client* owner
            , bt::NodeID infohash
            , shared_ptr<bt::MainlineDht> dht)
        : owner(owner)
        , dht(move(dht))
        , infohash(infohash)
    {}

    ~Bep5Loop() {
        wait_condition_locks.clear();
        cancel_();
    }

    void start() {
        asio::spawn(owner->get_io_service()
                   , [&] (asio::yield_context yield) {
                         Cancel cancel(cancel_);
                         sys::error_code ec;
                         loop(cancel, yield[ec]);
                     });
    }

    void loop(Cancel& cancel, asio::yield_context yield) {
        auto& ios = owner->get_io_service();

        while (!cancel) {
            sys::error_code ec;

            if (log_debug()) {
                LOG_DEBUG("Bep5Client: getting peers from swarm ", infohash);
            }

            auto endpoints = dht->tracker_get_peers(infohash, cancel, yield[ec]);

            assert(!cancel || ec == asio::error::operation_aborted);
            if (cancel) break;

            get_peers_call_count++;
            wait_condition_locks.clear();
            if (ec) {
                async_sleep(ios, 1s, cancel, yield);
                continue;
            }

            if (log_debug()) {
                LOG_DEBUG("Bep5Client: new endpoints: ", endpoints.size());
                for (auto ep: endpoints) {
                    LOG_DEBUG("Bep5Client:     ", ep);
                }
            }

            owner->add_injector_endpoints(move(endpoints));

            async_sleep(ios, 1min, cancel, yield);
        }
    }

    bool log_debug() {
        if (!owner) return false;
        return owner->_log_debug;
    }
};

struct Bep5Client::Client {
    unsigned fail_count = 0;
    std::unique_ptr<AbstractClient> client;

    Client(unique_ptr<AbstractClient> c) : client(move(c)) {}
};

Bep5Client::Bep5Client( shared_ptr<bt::MainlineDht> dht
                      , string swarm_name
                      , asio::ssl::context* tls_ctx)
    : _dht(dht)
    , _swarm_name(move(swarm_name))
    , _tls_ctx(tls_ctx)
    , _random_gen(std::time(0))
{
    if (_dht->local_endpoints().empty()) {
        LOG_ERROR("Bep5Client: DHT has no endpoints!");
    }
}

void Bep5Client::start(asio::yield_context)
{
    bt::NodeID infohash = util::sha1_digest(_swarm_name);

    LOG_INFO("Injector swarm: sha1('", _swarm_name, "'): ", infohash.to_hex());

    _bep5_loop.reset(new Bep5Loop(this, infohash, _dht));
    _bep5_loop->start();
}

void Bep5Client::stop()
{
    if (_bep5_loop) {
        _bep5_loop->wait_condition_locks.clear();
        _bep5_loop = nullptr;
    }
    _clients.clear();
}

static bool same_ipv(const udp::endpoint& ep1, const udp::endpoint& ep2)
{
    return ep1.address().is_v4() == ep2.address().is_v4();
}

boost::optional<asio_utp::udp_multiplexer>
Bep5Client::choose_multiplexer_for(const udp::endpoint& ep)
{
    auto eps = _dht->local_endpoints();

    for (auto& e : eps) {
        if (same_ipv(ep, e)) {
            asio_utp::udp_multiplexer m(get_io_service());
            sys::error_code ec;
            m.bind(e, ec);
            assert(!ec);
            return m;
        }
    }

    return boost::none;
}

unique_ptr<Bep5Client::Client> Bep5Client::build_client(const udp::endpoint& ep)
{
    auto opt_m = choose_multiplexer_for(ep);

    if (!opt_m) {
        LOG_ERROR("Bep5Client: Failed to choose multiplexer");
        return nullptr;
    }

    auto utp_client = make_unique<UtpOuiServiceClient>
        (get_io_service(), move(*opt_m), util::str(ep));

    if (!utp_client->verify_remote_endpoint()) {
        LOG_ERROR("Bep5Client: Failed to bind uTP client");
        return nullptr;
    }

    if (!_tls_ctx) {
        return make_unique<Client>(move(utp_client));
    }

    auto tls_client = make_unique<TlsOuiServiceClient>(move(utp_client), *_tls_ctx);

    return make_unique<Client>(move(tls_client));
}

void Bep5Client::add_injector_endpoints(set<udp::endpoint> eps)
{
    for (auto ep : eps) {
        if (bittorrent::is_martian(ep)) continue;
        auto r = _clients.emplace(ep, nullptr);
        if (r.second) {
            auto c = build_client(ep);
            if (!c) continue;
            r.first->second = move(c);
        }
    }
}

Bep5Client::Clients::iterator Bep5Client::choose_client()
{
    using Dist = std::uniform_int_distribution<unsigned>;

    if (_clients.empty()) return _clients.end();

    unsigned sum = 0;
    unsigned max = 0;

    std::vector<Clients::iterator> zero_fail_clients;

    for (auto i = _clients.begin(); i != _clients.end(); ++i) {
        if (i->second->fail_count == 0) {
            zero_fail_clients.push_back(i);
        }
    }

    if (zero_fail_clients.size()) {
        Dist dist(0, zero_fail_clients.size() - 1);
        return zero_fail_clients[dist(_random_gen)];
    }

    for (auto& inj : _clients) {
        max = std::max(max, inj.second->fail_count);
    }

    for (auto& inj : _clients) {
        unsigned n = (max - inj.second->fail_count) + 1;
        sum += n;
    }

    Dist dist(0, sum - 1);

    unsigned r = dist(_random_gen);

    for (auto i = _clients.begin(); i != _clients.end(); ++i) {
        unsigned n = (max - i->second->fail_count) + 1;
        if (n > r) return i;
        r -= n;
    }

    assert(0);
    return _clients.end();
}

unsigned Bep5Client::lowest_fail_count() const
{
    unsigned ret = std::numeric_limits<unsigned>::max();
    for (auto& ep : _clients) ret = std::min(ret, ep.second->fail_count);
    return ret;
}

GenericStream Bep5Client::connect(asio::yield_context yield, Cancel& cancel_)
{
    Cancel cancel(cancel_);
    auto cancel_con = _cancel.connect([&] { cancel(); });

    if (_wait_for_bep5_resolve && _bep5_loop->get_peers_call_count == 0) {
        WaitCondition wc(_dht->get_io_service());

        _bep5_loop->wait_condition_locks.push_back(wc.lock());

        sys::error_code ec;
        wc.wait(cancel, yield[ec]);

        if (cancel)
            return or_throw<GenericStream>(yield, asio::error::operation_aborted);
    }

    while (lowest_fail_count() < 5) {
        auto i = choose_client();

        if (_log_debug) LOG_DEBUG("Connecting...");

        if (i == _clients.end()) {
            if (_log_debug) LOG_DEBUG("Connect failed: no remote endpoints");
            return or_throw<GenericStream>(yield, asio::error::host_unreachable);
        }

        auto p = i->second.get();

        auto ep = i->first;

        if (_log_debug) LOG_DEBUG("Connecting to: ", ep);

        sys::error_code ec;
        auto con = p->client->connect(yield[ec], cancel);

        if (_log_debug) LOG_DEBUG("Connecting to: ", ep, " done: ", ec.message());

        if (cancel)
            return or_throw<GenericStream>(yield, asio::error::operation_aborted);

        if (!ec) {
            p->fail_count = 0;
            return con;
        }

        p->fail_count++;
    } 

    return or_throw<GenericStream>(yield, asio::error::host_unreachable);
}

Bep5Client::~Bep5Client()
{
    stop();
}

asio::io_service& Bep5Client::get_io_service()
{
    return _dht->get_io_service();
}
