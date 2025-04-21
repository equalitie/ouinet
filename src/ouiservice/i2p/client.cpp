#include <chrono>

#include <I2PTunnel.h>
#include <I2PService.h>

#include "client.h"
#include "service.h"
#include "handshake.h"

#include "../../async_sleep.h"
#include "../../bittorrent/node_id.h"
#include "../../logger.h"
#include "../../namespaces.h"
#include "../../or_throw.h"
#include "../../util/hash.h"
#include "../../util/lru_cache.h"
#include "../../util/wait_condition.h"

#define _LOGPFX "I2PClient: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _VERBOSE(...)  LOG_VERBOSE(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

// It is probably good to drop entries more aggressively from here
// to avoid accumulating spurious fake injector entries
// which may impede trying to ping good injectors.
static const std::size_t injector_swarm_capacity = 50;

namespace ouinet::ouiservice::i2poui {

using namespace std;

namespace bt = bittorrent;

struct Client::Swarm
{
private:
    using Peer  = AbstractClient;
    using Peers = util::LruCache<asio::ip::udp::endpoint, std::shared_ptr<Peer>>;

private:
    Client* _owner;
    //shared_ptr<bt::MainlineDht> _dht;
    bt::NodeID _infohash;
    Cancel _lifetime_cancel;
    size_t _get_peers_call_count = 0;
    std::vector<WaitCondition::Lock> _wait_condition_locks;
    Peers _peers;
    const bool _connect_proxy;

public:
    Swarm( Client* owner
         , bt::NodeID infohash
         //, shared_ptr<bt::MainlineDht> dht
         , size_t capacity
         , Cancel& cancel
         , bool connect_proxy)
        : _owner(owner)
        //, _dht(move(dht)) // TODO: replace with bep3 tracker
        , _infohash(infohash)
        , _lifetime_cancel(cancel)
        , _peers(capacity)
        , _connect_proxy(connect_proxy)
    {}

    ~Swarm() {
        _wait_condition_locks.clear();
        _lifetime_cancel();
    }

    /*
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

            auto r = _peers.get(ep);
            if (r) continue;  // already known, moved to front
            auto p = make_peer(ep);
            if (!p) continue;
            _peers.put(ep, move(p));
        }
    }
    */
};

Client::Client(std::shared_ptr<Service> service, const string& target_id, uint32_t timeout, const AsioExecutor& exec)
    : _service(service)
    , _exec(exec)
    , _target_id(target_id)
    , _timeout(timeout)
{}

Client::~Client()
{
    stop();
}

void Client::start(asio::yield_context yield)
{
    Cancel stopped = _stopped;

    sys::error_code ec;

    do {
        auto i2p_client_tunnel = std::make_unique<i2p::client::I2PClientTunnel>(
                "i2p_oui_client",
                _target_id,
                "127.0.0.1",
                0,
                _service ? _service->get_local_destination () : nullptr);

        _client_tunnel = std::make_unique<Tunnel>(_exec, std::move(i2p_client_tunnel), _timeout);

        _client_tunnel->wait_to_get_ready(yield[ec]);
    } while(_client_tunnel->has_timed_out() && !stopped);

    if (!ec && !_client_tunnel) ec = asio::error::operation_aborted;
    ec = compute_error_code(ec, stopped);
    if (ec) return or_throw(yield, ec);

    _port = _client_tunnel->local_endpoint().port();

    {
        bt::NodeID infohash = util::sha1_digest(_injector_swarm_name);

        _INFO("Injector swarm: sha1('", _injector_swarm_name, "'): ", infohash.to_hex());

        _injector_swarm.reset(new Swarm(this, infohash, injector_swarm_capacity, _stopped, false));
        //_injector_swarm->start();
    }

}

void Client::stop()
{
    _client_tunnel.reset();
    //tunnel destructor will stop the i2p tunnel after the connections
    //are closed. (TODO: maybe we need to add a wait here)
    _stopped();
}

inline void exponential_backoff(AsioExecutor& exec, uint32_t i, Cancel& cancel, asio::yield_context yield) {
    // Constants in this function are made up, feel free to modify them as needed.
    if (i < 3) return;
    i -= 3;
    uint32_t constant_after = 8; // max 12.8 seconds
    if (i > constant_after) i = constant_after;
    float delay_s = powf(2, i) / 10.f;

    if (!async_sleep(exec, chrono::milliseconds(long(delay_s * 1000.f)), cancel, yield)) {
        return or_throw(yield, asio::error::operation_aborted);
    }
}

::ouinet::GenericStream
Client::connect(asio::yield_context yield, Cancel& cancel)
{
    for (uint32_t i = 0;; ++i) {
        sys::error_code ec;
        auto conn = connect_without_handshake(yield[ec], cancel);

        if (!ec) {
            auto stopped = _stopped.connect([&cancel] { cancel(); });
            perform_handshake(conn, cancel, yield[ec]);

            if (!ec) {
                return conn;
            }
        }

        if (ec == asio::error::operation_aborted) {
            return or_throw<GenericStream>(yield, ec);
        }

        assert(ec);

        ec = {};
        exponential_backoff(_exec, i, cancel, yield[ec]);

        if (ec) {
            return or_throw<GenericStream>(yield, ec);
        }
    }
}

::ouinet::GenericStream
Client::connect_without_handshake(asio::yield_context yield, Cancel& cancel)
{
    auto stopped = _stopped.connect([&cancel] { cancel(); });

    Connection connection(_exec);
    
    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        connection.close();
    });

    LOG_DEBUG("Connecting to the i2p injector...");

    for (uint32_t i = 0;; ++i) {
        sys::error_code ec;

        connection._socket.async_connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), _port), yield[ec]);
        ec = compute_error_code(ec, cancel);

        if (ec == asio::error::operation_aborted) {
            return or_throw<GenericStream>(yield, ec);
        }

        if (ec) {
            ec = {};
            exponential_backoff(_exec, i, cancel, yield[ec]);
            if (ec) return or_throw<GenericStream>(yield, ec);
            continue;
        }

        LOG_DEBUG("Connection to the i2p injector is established");

        _client_tunnel->intrusive_add(connection);

        return GenericStream{move(connection)};
    }
}

} // namespaces
