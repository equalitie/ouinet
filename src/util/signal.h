#pragma once

#include <functional>
#include <iostream>

#include <boost/asio/error.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/system/error_code.hpp>

#include "../namespaces.h"

#include "../or_throw.h"

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
            , _call_count(other._call_count)
        {
            other._call_count = 0;
            other.swap_nodes(*this);
        }

        Connection& operator=(Connection&& other) {
            _slot = std::move(other._slot);
            _call_count = other._call_count;
            other._call_count = 0;
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
    Signal()                    = default;

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;

    Signal(Signal& parent)
        : _parent_connection(parent.connect(call_to_self()))
    {}

    Signal(Signal&& other)
        : _connections(std::move(other._connections))
        , _call_count(other._call_count)
    {
        other._call_count = 0;

        if (other._parent_connection._slot) {
            _parent_connection = std::move(other._parent_connection);
            _parent_connection._slot = call_to_self();
        }
    }

    Signal& operator=(Signal&& other)
    {
        _connections = std::move(other._connections);
        _call_count = other._call_count;
        other._call_count = 0;

        if (other._parent_connection._slot) {
            _parent_connection = std::move(other._parent_connection);
            _parent_connection._slot = call_to_self();
        }

        return *this;
    }

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
    auto call_to_self() {
        return [&] (auto&&... args) {
                    (*this)(std::forward<decltype(args)>(args)...);
               };
    }

private:
    List<Connection> _connections;
    size_t _call_count = 0;
    Connection _parent_connection;
};

// This is how we use it 99% (100%?) of the time.
using Cancel = Signal<void()>;

inline
sys::error_code
compute_error_code( const sys::error_code& ec
                  , const Cancel& cancel)
{
    // The point of having this function is to correct this behavior,
    // so that it can be used after any async call
    // regardless of whether it knows about signals or not.
    //assert(!cancel || ec == asio::error::operation_aborted);
    if (cancel) return asio::error::operation_aborted;
    return ec;
}

// Doing error checking is quite cumbersome. One has to check whether `cancel`
// is true, make sure that if `cancel` is indeed true, that ec is set
// appropriately and then return if any of the two is set. Instead of doing it
// after each async operation, this macro is ought to help with it.
//
// Usage:
//
// int foo(Cancel& cancel, yield_context yield) {
//     sys::error_code ec;
//     int ret = my_async_operation(cancel, yield[ec]);
//     return_or_throw_on_error(yield, cancel, ec, int(0));
//
//     // ... other async operations
//
//     return ret;
// }
//
#define return_or_throw_on_error(yield, cancel, ec, ...) { \
    sys::error_code ec_ = compute_error_code(ec, cancel); \
    if (ec_) return or_throw(yield, ec_, ##__VA_ARGS__); \
}

} // ouinet namespace
