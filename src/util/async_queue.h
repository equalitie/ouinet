#pragma once

#include <queue>
#include "condition_variable.h"
#include "../or_throw.h"

namespace ouinet { namespace util {

template<class T, template<typename...> class Q = std::deque> class AsyncQueue {
private:
    using Queue = Q<std::pair<T, sys::error_code>>;

public:
    using iterator       = typename Queue::iterator;
    using const_iterator = typename Queue::const_iterator;

public:
    AsyncQueue(asio::io_context& ctx, size_t max_size = -1)
        : AsyncQueue(ctx.get_executor(), max_size)
    {}

    AsyncQueue(const AsioExecutor& ex, size_t max_size = -1)
        : _ex(ex)
        , _max_size(max_size)
        , _rx_cv(_ex)
        , _tx_cv(_ex)
    {}

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;

    // Might be possible, but non-trivial (I think)
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    void insert(iterator pos, const T& value)
    {
        _queue.insert(pos, {std::move(value), {}});
        _rx_cv.notify();
    }

    void async_push(T val, Cancel cancel, asio::yield_context yield)
    {
        async_push(std::move(val), sys::error_code(), std::move(cancel), yield);
    }

    void async_push( T val
                   , sys::error_code ec_
                   , Cancel cancel
                   , asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });

        sys::error_code ec;

        while (_queue.size() >= _max_size) {
            _tx_cv.wait(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec);
        }

        _queue.push_back({std::move(val), ec_});
        _rx_cv.notify();
    }

    // Deprecated, use push_back
    void push(T val)
    {
        _queue.push_back({std::move(val), sys::error_code{}});
        _rx_cv.notify();
    }

    void push_back(T val)
    {
        _queue.push_back({std::move(val), sys::error_code{}});
        _rx_cv.notify();
    }

    void push_front(T val)
    {
        _queue.push_front({std::move(val), sys::error_code{}});
        _rx_cv.notify();
    }

    template<class Range>
    void async_push_many( const Range& range
                        , Cancel cancel
                        , asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });

        sys::error_code ec;

        auto i = std::begin(range);
        auto end = std::end(range);

        while (i != end) {
            while (_queue.size() >= _max_size) {
                _tx_cv.wait(cancel, yield[ec]);
                return_or_throw_on_error(yield, cancel, ec);
            }

            while (_queue.size() < _max_size && i != end) {
                _queue.push_back({*i, sys::error_code()});
                ++i;
            }

            _rx_cv.notify();
        }
    }

    void async_wait_for_push(Cancel cancel, asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });
        sys::error_code ec;
        _rx_cv.wait(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }

    T async_pop(Cancel cancel, asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });

        sys::error_code ec;

        while (_queue.empty()) {
            _rx_cv.wait(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, T{});
        }

        assert(!_queue.empty());

        auto ret = std::move(_queue.front());
        _queue.pop_front();

        _tx_cv.notify();

        return or_throw<T>(yield, ret.second, std::move(ret.first));
    }

    size_t async_flush( std::queue<T>& out
                      , Cancel cancel
                      , asio::yield_context yield)
    {
        auto slot = _destroy_signal.connect([&] { cancel(); });

        sys::error_code ec;

        while (_queue.empty()) {
            _rx_cv.wait(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, 0);
        }

        assert(!_queue.empty());

        size_t ret = 0;

        while (!_queue.empty()) {
            auto p = std::move(_queue.front());
            _queue.pop_front();
            if (!p.second) {
                ++ret;
                out.push(std::move(p.first));
            }
        }

        _tx_cv.notify();

        ec = compute_error_code(ec, cancel);
        return or_throw<size_t>(yield, ec, 0);
    }

    T& back() {
        assert(!_queue.empty());
        return _queue.back().first;
    }

    T& front() {
        assert(!_queue.empty());
        return _queue.front().first;
    }

    void pop() {
        assert(!_queue.empty());
        _queue.pop_front();
        _tx_cv.notify();
    }

    iterator erase(iterator i)
    {
        iterator r = _queue.erase(i);
        _tx_cv.notify();
        return r;
    }

    ~AsyncQueue()
    {
        _destroy_signal();
    }

    size_t size() const { return _queue.size(); }
    bool empty() const { return _queue.empty(); }

    iterator begin() { return _queue.begin(); }
    iterator end()   { return _queue.end();   }

    const_iterator begin() const { return _queue.begin(); }
    const_iterator end()   const { return _queue.end();   }

    AsioExecutor get_executor()
    {
        return _ex;
    }

private:
    AsioExecutor _ex;
    size_t _max_size;
    Queue _queue;
    ConditionVariable _rx_cv;
    ConditionVariable _tx_cv;
    Cancel _destroy_signal;
};

}} // namespaces
