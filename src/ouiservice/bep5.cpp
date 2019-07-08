#include "bep5.h"
#include "utp.h"
#include "tls.h"
#include "../async_sleep.h"
#include "../bittorrent/dht.h"
#include "../bittorrent/bep5_announcer.h"
#include "../bittorrent/is_martian.h"
#include "../logger.h"
#include "../util/hash.h"

using namespace std;
using namespace ouinet;
using namespace ouiservice;

namespace bt = bittorrent;

using AbstractClient = OuiServiceImplementationClient;
using AbstractServer = OuiServiceImplementationServer;
using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

//////////////////////////////////////////////////////////////////////
// Server

using namespace std::chrono_literals;

struct Bep5Server::State
{
    State( shared_ptr<bt::MainlineDht> dht
         , bt::NodeID infohash
         , unique_ptr<AbstractServer> srv)
        : dht(move(dht))
        , server(move(srv))
        , infohash(infohash)
    {
    }

    void start( util::AsyncQueue<GenericStream>& accept_queue
              , Cancel& outer_cancel
              , asio::yield_context yield)
    {
        announcer = bt::Bep5Announcer(infohash, dht);

        auto& ios = dht->get_io_service();

        sys::error_code ec;
        server->start_listen(yield[ec]);
        assert(!ec);

        Cancel cancel(outer_cancel);

        asio::spawn(ios, [&, cancel = move(cancel)]
                (asio::yield_context yield) mutable {
            while (!cancel) {
                sys::error_code ec;
                auto con = server->accept(yield[ec]);

                if (cancel) break;

                if (ec) {
                    async_sleep(ios, 100ms, cancel, yield);
                    if (cancel) break;
                    continue;
                }

                accept_queue.async_push(move(con), ec, cancel, yield[ec]);
                assert(!cancel && !ec);
            }
        });
    }

    std::shared_ptr<bt::MainlineDht> dht;
    bt::Bep5Announcer announcer;
    std::unique_ptr<AbstractServer> server;
    bt::NodeID infohash;
};

Bep5Server::Bep5Server( shared_ptr<bt::MainlineDht> dht
                      , boost::asio::ssl::context* ssl_context
                      , string swarm_name)
    : _dht(dht)
    , _accept_queue(_dht->get_io_service())
{
    assert(_dht);

    auto& ios = _dht->get_io_service();

    auto endpoints = _dht->local_endpoints();

    if (endpoints.empty()) {
        LOG_ERROR("Bep5Server: DHT has no endpoints!");
    }

    bt::NodeID infohash = util::sha1_digest(swarm_name);
    LOG_INFO("Injector swarm: sha1('", swarm_name, "'): ", infohash.to_hex());

    for (auto ep : endpoints) {
        auto base = make_unique<ouiservice::UtpOuiServiceServer>(ios, ep);
        if (ssl_context) {
            LOG_INFO("Bep5: uTP/TLS Address: ", ep);
            auto tls = make_unique<ouiservice::TlsOuiServiceServer>(ios, move(base), *ssl_context);
            _states.emplace_back(new State(dht, infohash, move(tls)));
        } else {
            LOG_INFO("Bep5: uTP Address: ", ep);
            _states.emplace_back(new State(dht, infohash, move(base)));
        }
    }
}

void Bep5Server::start_listen(asio::yield_context yield)
{
    for (auto& s : _states) {
        sys::error_code ec;
        s->start(_accept_queue, _cancel, yield[ec]);
        if (ec) {
            LOG_ERROR("Bep5Server: Failed to start listen: ", ec.message());
        }
    }
}

void Bep5Server::stop_listen()
{
    _cancel();
    _states.clear();
}

GenericStream Bep5Server::accept(asio::yield_context yield)
{
    sys::error_code ec;
    auto s = _accept_queue.async_pop(_cancel, yield[ec]);
    return or_throw(yield, ec, move(s));
}

Bep5Server::~Bep5Server()
{
    stop_listen();
}

//////////////////////////////////////////////////////////////////////
// Client
static set<udp::endpoint> tcp_to_udp(const set<tcp::endpoint>& eps)
{
    set<udp::endpoint> ret;
    for (auto& ep : eps) { ret.insert({ep.address(), ep.port()}); }
    return ret;
}

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

            owner->add_injector_endpoints(tcp_to_udp(endpoints));

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

void Bep5Client::add_injector_endpoints(const set<udp::endpoint>& eps)
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
    if (_clients.empty()) return _clients.end();

    unsigned sum = 0;
    unsigned max = 0;

    for (auto& inj : _clients) {
        max = std::max(max, inj.second->fail_count);
    }

    for (auto& inj : _clients) {
        unsigned n = (max - inj.second->fail_count) + 1;
        sum += n;
    }

    std::uniform_int_distribution<unsigned> dist(0, sum - 1);

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
