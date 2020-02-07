#include "reachability.h"
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/optional.hpp>
#include <chrono>
#include "condition_variable.h"

namespace ouinet { namespace util {

/*
 * Stores a map<endpoint, time_point>, ordered by time_point.
 */
class ConnectionTracker {
public:
    using Key = asio_utp::udp_multiplexer::endpoint_type;
    using Value = std::chrono::steady_clock::time_point;

private:
    struct Connection {
        Key key;
        Value value;
        boost::intrusive::set_member_hook<> key_order_hook;
        boost::intrusive::set_member_hook<> value_order_hook;
    };

public:
    ConnectionTracker() {}
    ~ConnectionTracker();

    ConnectionTracker(const ConnectionTracker&) = delete;
    ConnectionTracker(ConnectionTracker&&) = delete;
    ConnectionTracker& operator=(const ConnectionTracker&) = delete;
    ConnectionTracker& operator=(ConnectionTracker&&) = delete;

    bool empty() const { return _connection_set_by_key.empty(); }
    bool contains(const Key& key) const { return !!get(key); }
    boost::optional<Value> get(const Key& key) const;
    boost::optional<std::pair<Key, Value>> first_entry_by_value() const;

    void insert(const Key& key, const Value& value);
    void remove(const Key& key);

private:
    struct CompareByKey {
        bool operator()(const Connection& a, const Connection& b) const { return a.key < b.key; }
    };
    struct CompareByValue {
        bool operator()(const Connection& a, const Connection& b) const { return a.value < b.value; }
    };

    boost::intrusive::set<
        Connection,
        boost::intrusive::member_hook<
            Connection,
            boost::intrusive::set_member_hook<>,
            &Connection::key_order_hook
        >,
        boost::intrusive::compare<CompareByKey>
    > _connection_set_by_key;

    boost::intrusive::multiset<
        Connection,
        boost::intrusive::member_hook<
            Connection,
            boost::intrusive::set_member_hook<>,
            &Connection::value_order_hook
        >,
        boost::intrusive::compare<CompareByValue>
    > _connection_multiset_by_value;
};

inline ConnectionTracker::~ConnectionTracker()
{
    while (true) {
        boost::optional<std::pair<Key, Value>> pair = first_entry_by_value();
        if (pair) {
            remove(pair->first);
        } else {
            break;
        }
    }
}

inline boost::optional<ConnectionTracker::Value> ConnectionTracker::get(const ConnectionTracker::Key& key) const
{
    Connection comparison_target;
    comparison_target.key = key;
    auto it = _connection_set_by_key.find(comparison_target);
    if (it == _connection_set_by_key.end()) {
        return boost::none;
    } else {
        return it->value;
    }
}

inline boost::optional<std::pair<ConnectionTracker::Key, ConnectionTracker::Value>> ConnectionTracker::first_entry_by_value() const
{
    auto it = _connection_multiset_by_value.begin();
    if (it == _connection_multiset_by_value.end()) {
        return boost::none;
    } else {
        std::pair<Key, Value> pair;
        pair.first = it->key;
        pair.second = it->value;
        return pair;
    }
}

void ConnectionTracker::insert(const ConnectionTracker::Key& key, const ConnectionTracker::Value& value)
{
    if (contains(key)) {
        remove(key);
    }

    Connection* connection = new Connection;
    connection->key = key;
    connection->value = value;
    _connection_set_by_key.insert(*connection);
    _connection_multiset_by_value.insert(*connection);
}

void ConnectionTracker::remove(const ConnectionTracker::Key& key)
{
    Connection comparison_target;
    comparison_target.key = key;
    auto it = _connection_set_by_key.find(comparison_target);
    if (it == _connection_set_by_key.end()) {
        return;
    }
    Connection* connection = &*it;
    _connection_set_by_key.erase(*connection);
    _connection_multiset_by_value.erase(*connection);
    delete connection;
}



struct UdpServerReachabilityAnalysis::State {
    asio_utp::udp_multiplexer multiplexer;
    Reachability judgement;
    Signal<void()> on_judgement_change;

    ConnectionTracker connections;
    std::chrono::steady_clock::time_point last_unsolicited_traffic;
    std::chrono::steady_clock::time_point startup_uncertaincy_expiracy;
    asio_utp::udp_multiplexer::on_send_to_connection on_send_to_connection;
    Signal<void()> on_destroy;

    void cleanup_connections(std::chrono::steady_clock::time_point now);
};

void UdpServerReachabilityAnalysis::State::cleanup_connections(std::chrono::steady_clock::time_point now)
{
    while (true) {
        boost::optional<std::pair<ConnectionTracker::Key, ConnectionTracker::Value>> pair = connections.first_entry_by_value();
        if (!pair) {
            return;
        }

        if (pair->second >= now) {
            return;
        }

        connections.remove(pair->first);
    }
}



UdpServerReachabilityAnalysis::UdpServerReachabilityAnalysis()
{}

UdpServerReachabilityAnalysis::~UdpServerReachabilityAnalysis()
{
    stop();
}

void UdpServerReachabilityAnalysis::start(const asio::executor& executor, const asio_utp::udp_multiplexer& udp_socket)
{
    if (_state) {
        stop();
    }

    _state = std::make_shared<State>();
    sys::error_code ec_ignored;
    _state->multiplexer.bind(udp_socket, ec_ignored);
    _state->judgement = Reachability::Unreachable;
    _state->startup_uncertaincy_expiracy = std::chrono::steady_clock::now() + std::chrono::seconds(long(lingeringConnectionTrackingTime));

    /*
     * Listen to incoming datagrams. Track active connections using ConnectionTracker.
     * When receiving a datagram not in the tracker, judge Reachable.
     */
    asio::spawn(executor, [state = _state] (asio::yield_context yield) {
        bool running = true;
        auto connection = state->on_destroy.connect([&running, state = state.get()] {
            running = false;
            sys::error_code ec;
            state->multiplexer.close(ec);
        });

        while (running) {
            unsigned char buffer[64];
            asio_utp::udp_multiplexer::endpoint_type endpoint;
            sys::error_code ec;

            state->multiplexer.async_receive_from(
                asio::mutable_buffer(buffer, sizeof(buffer)),
                endpoint,
                yield[ec]
            );

            if (!ec) {
                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                state->cleanup_connections(now);
                bool found = state->connections.contains(endpoint);
                state->connections.insert(endpoint, now + std::chrono::seconds(long(connectionTrackingExpiracyTime)));

                if (!found) {
                    state->last_unsolicited_traffic = now;

                    Reachability next_judgement =
                        (state->startup_uncertaincy_expiracy < now) ?
                        Reachability::ConfirmedReachable :
                        Reachability::UnconfirmedReachable;
                    if (state->judgement != next_judgement) {
                        state->judgement = next_judgement;
                        state->on_judgement_change();
                    }
                }
            }
        }
    });

    /*
     * Track outgoing datagrams in ConnectionTracker.
     */
    _state->on_send_to_connection = _state->multiplexer.on_send_to([state = _state.get()] (
        const std::vector<boost::asio::const_buffer>& buffer,
        size_t length,
        const asio_utp::udp_multiplexer::endpoint_type& endpoint,
        sys::error_code ec
    ) {
        if (!ec) {
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            state->cleanup_connections(now);
            state->connections.insert(endpoint, now + std::chrono::seconds(long(connectionTrackingExpiracyTime)));
        }
    });

    /*
     * Wait for (last_unsolicited_traffic) + (expiracy time), then downgrade judgement.
     */
    asio::spawn(executor, [state = _state, executor] (asio::yield_context yield) {
        ConditionVariable reachable_condition(executor);
        asio::steady_timer timer(executor);
        bool running = true;

        auto judgement_change_connection = state->on_judgement_change.connect([&] {
            reachable_condition.notify();
            timer.cancel();
        });

        auto destroy_connection = state->on_destroy.connect([&] {
            running = false;
            reachable_condition.notify();
            timer.cancel();
        });

        while (running) {
            sys::error_code ec;
            if (state->judgement == Reachability::Unreachable) {
                reachable_condition.wait(yield[ec]);
            } else {
                long expiracy_time;
                Reachability next_judgement;
                if (state->judgement == Reachability::ConfirmedReachable) {
                    expiracy_time = confirmedReachabilityExpiracyTime;
                    next_judgement = Reachability::UnconfirmedReachable;
                } else if (state->judgement == Reachability::UnconfirmedReachable) {
                    expiracy_time = unconfirmedReachabilityExpiracyTime;
                    next_judgement = Reachability::Unreachable;
                } else {
                    assert(false);
                }

                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                std::chrono::steady_clock::time_point next_downgrade =
                    state->last_unsolicited_traffic + std::chrono::seconds(expiracy_time);

                if (next_downgrade < now) {
                    state->last_unsolicited_traffic = next_downgrade;
                    state->judgement = next_judgement;
                    state->on_judgement_change();
                } else {
                    timer.expires_at(next_downgrade);
                    timer.async_wait(yield[ec]);
                }
            }
        }
    });
}

void UdpServerReachabilityAnalysis::stop()
{
    if (_state) {
        /*
         * Clear out signal
         */
        Signal<void()> empty_on_judgement_change;
        _state->on_judgement_change = std::move(empty_on_judgement_change);

        _state->on_destroy();

        _state.reset();
    }
}

UdpServerReachabilityAnalysis::Reachability UdpServerReachabilityAnalysis::judgement() const
{
    if (_state) {
        return _state->judgement;
    } else {
        return Reachability::Unreachable;
    }
}

Signal<void()>& UdpServerReachabilityAnalysis::on_judgement_change()
{
    assert(_state);
    return _state->on_judgement_change;
}



}} // namespaces
