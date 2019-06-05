#pragma once

#include <functional>

#include <boost/intrusive/list.hpp>

namespace ouinet {

template<typename T>
class Signal {
private:
    template<class K>
    using List = boost::intrusive::list<K, boost::intrusive::constant_time_size<false>>;
    using Hook = boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

public:
    class Connection : public Hook
    {
    public:
        Connection() = default;

        Connection(Connection&& other)
            : _slot(std::move(other._slot))
        {
            other.swap_nodes(*this);
        }

        Connection& operator=(Connection&& other) {
            _slot = std::move(other._slot);
            other.swap_nodes(*this);
            return *this;
        }

        size_t call_count() const { return _call_count; }

        operator bool() const { return call_count() != 0; }

    private:
        friend class Signal;
        std::function<T> _slot;
        size_t _call_count = 0;
    };

public:
    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) = default;
    Signal& operator=(Signal&&) = default;

    Signal(Signal& parent)
        : _parent_connection(parent.connect([&] (auto&&... args) {
                    (*this)(std::forward<decltype(args)>(args)...);
                }))
    {}

    template<typename... Args>
    void operator()(Args&&... args)
    {
        ++_call_count;

        auto connections = std::move(_connections);
        for (auto& connection : connections) {
            try {
                ++connection._call_count;
                connection._slot(std::forward<Args>(args)...);
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
        connection._slot = std::move(slot);
        _connections.push_back(connection);
        return connection;
    }

    size_t size() const { return _connections.size(); }

private:
    List<Connection> _connections;
    size_t _call_count = 0;
    Connection _parent_connection;
};

// This is how we use it 99% (100%?) of the time.
using Cancel = Signal<void()>;

} // ouinet namespace
