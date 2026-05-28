#pragma once

#include <functional>
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include "intrusive_list.h"
#include "../namespaces.h"
#include "../or_throw.h"

namespace ouinet {

class Cancel {
public:
    class Connection
    {
    public:
        Connection() = default;

        Connection(Connection&& other) {
            *this = std::move(other);
        }

        Connection& operator=(Connection&& other) {
            if (this == &other) return *this;
            _slot = std::move(other._slot);
            other._hook.swap_nodes(this->_hook);
            other._hook.unlink();
            return *this;
        }

        operator bool() const { return !(bool) _slot; }

    private:
        friend class Cancel;

        void on_signal() {
            if (_slot) {
                auto slot = std::move(_slot);
                slot();
            }
        }

    private:
        util::intrusive::list_hook _hook;
        std::function<void()> _slot;
    };

public:
    Cancel() = default;

    Cancel& operator=(const Cancel&) = delete;

    Cancel(const Cancel& parent)
        : _parent(const_cast<Cancel*>(&parent))
        , _ec(parent._ec)
    {
        _parent->_children.push_back(*this);
    }

    Cancel(Cancel&& other) {
        *this = std::move(other);
    }

    Cancel& operator=(Cancel&& other)
    {
        if (this == &other) return *this;

        abandon_children();

        _hook.unlink();
        _hook.swap_nodes(other._hook);

        _parent = other._parent;
        other._parent = nullptr;

        _children.swap(other._children);
        other._children.clear();

        _connections.swap(other._connections);
        other._connections.clear();

        _ec = other._ec;
        other._ec = sys::error_code();

        for (auto& c : _children) {
            c._parent = this;
        }

        return *this;
    }

    void operator()(sys::error_code ec = asio::error::operation_aborted)
    {
        assert(ec != sys::error_code() && "Don't cancel with success");

        if (_ec == asio::error::operation_aborted) {
            // Already called and nothing overrides `operation_aborted`.
            return;
        }

        _ec = ec;

        auto cs = std::move(_connections);

        for (auto& c : cs) { c.on_signal(); }
        for (auto& c : _children) { c(ec); }
    }

    sys::error_code error_code() const {
        return _ec;
    }

    // True when `operator()` was called.
    operator bool() const { return (bool) _ec; }

    [[nodiscard]]
    Connection connect(std::function<void()> slot)
    {
        Connection connection;
        connection._slot = std::move(slot);
        _connections.push_back(connection);
        return connection;
    }

    size_t size() const { return _connections.size(); }

    ~Cancel() {
        abandon_children();
    }

private:
    // Move children to their grand parent (parent of this).
    void abandon_children() {
        if (_parent) {
            assert(_hook.is_linked());
            auto i = _parent->_children.iterator_to(*this);
            while (!_children.empty()) {
                auto& c = _children.front();
                c._hook.unlink();
                c._parent = _parent;
                _parent->_children.insert(i, c);
            }
        }
        else {
            for (auto& c : _children) {
                c._parent = nullptr;
            }
        }
    }

private:
    util::intrusive::list_hook _hook;
    Cancel* _parent = nullptr;
    util::intrusive::list<Cancel, &Cancel::_hook> _children;
    util::intrusive::list<Connection, &Connection::_hook> _connections;
    sys::error_code _ec;
};

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
