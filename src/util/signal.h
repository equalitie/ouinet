#pragma once

#include <functional>

#include <boost/intrusive/list.hpp>

namespace ouinet {

template<typename T>
class Signal {
private:
    using Hook = boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

public:
    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    class Connection : public Hook
    {
    public:
        Connection() = default;

        Connection(Connection&& other)
            : slot(std::move(other.slot))
        {
            other.swap_nodes(*this);
        }

        Connection& operator=(Connection&& other) {
            slot = std::move(other.slot);
            other.swap_nodes(*this);
            return *this;
        }

    private:
        friend class Signal;
        std::function<T> slot;
    };

    template<typename... Args>
    void operator()(Args&&... args)
    {
        ++_call_count;

        auto connections = std::move(_connections);
        for (auto& connection : connections) {
            try {
                connection.slot(std::forward<Args>(args)...);
            } catch (std::exception& e) {
                assert(0);
            }
        }
    }

    size_t call_count() const { return _call_count; }

    operator bool() const { return call_count() != 0; }

    Connection connect(std::function<T> slot)
    {
        Connection connection;
        connection.slot = std::move(slot);
        _connections.push_back(connection);
        return connection;
    }

    size_t size() const { return _connections.size(); }

private:
    boost::intrusive::list<Connection, boost::intrusive::constant_time_size<false>> _connections;
    size_t _call_count = 0;
};

// This is how we use it 99% (100%?) of the time.
using Cancel = Signal<void()>;

} // ouinet namespace
