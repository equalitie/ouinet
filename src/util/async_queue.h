#pragma once

#include <queue>
#include "condition_variable.h"
#include "util/async.h"

namespace ouinet::util {

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

    AsyncQueue(asio::any_io_executor ex, size_t max_size = -1)
        : _ex(std::move(ex))
        , _max_size(max_size)
        , _rx_cv(_ex)
        , _tx_cv(_ex)
    {}

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;

    // TODO
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    void insert(iterator pos, const T& value)
    {
        _queue.insert(pos, {std::move(value), {}});
        _rx_cv.notify();
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_push(T val, sys::error_code ec, Async yield)
    {
        auto slot = _destroy_signal.connect([&] { yield.cancel(); });

        while (_queue.size() >= _max_size) {
            auto r = _tx_cv.wait(yield);
            if (!r) return r;
        }

        _queue.push_back({std::move(val), ec});
        _rx_cv.notify();

        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_push(T val, Async yield)
    {
        return async_push(std::move(val), sys::error_code{}, yield);
    }

    // Does not block, may temporarily increase the queue size until popped.
    void push_back(T val)
    {
        push_back(std::move(val), sys::error_code{});
    }

    // Does not block, may temporarily increase the queue size until popped.
    void push_back(T val, sys::error_code ec)
    {
        _queue.push_back({std::move(val), ec});
        _rx_cv.notify();
    }

    // Does not block, may temporarily increase the queue size until popped.
    void push_front(T val)
    {
        _queue.push_front({std::move(val), sys::error_code{}});
        _rx_cv.notify();
    }

    template<class Range>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_push_many(const Range& range, Async yield)
    {
        auto slot = _destroy_signal.connect([&] { yield.cancel(); });

        auto i = std::begin(range);
        auto end = std::end(range);

        while (i != end) {
            while (_queue.size() >= _max_size) {
                auto r = _tx_cv.wait(yield);
                if (!r) return r;
            }

            while (_queue.size() < _max_size && i != end) {
                _queue.push_back({*i, sys::error_code()});
                ++i;
            }

            _rx_cv.notify();
        }

        return {};
    }

    // Wait for push, but don't pop
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_wait_for_push(Async yield) {
        auto slot = _destroy_signal.connect([&] { yield.cancel(); });
        return _rx_cv.wait(yield);
    }

    [[nodiscard]]
    std::expected<T, sys::error_code>
    async_pop(Async yield)
    {
        auto slot = _destroy_signal.connect([&] { yield.cancel(); });

        while (_queue.empty()) {
            auto r = _rx_cv.wait(yield);
            if (!r) return std::unexpected(r.error());
        }

        assert(!_queue.empty());

        auto ret = std::move(_queue.front());
        _queue.pop_front();

        _tx_cv.notify();

        if (ret.second) return std::unexpected(ret.second);
        return {std::move(ret.first)};
    }

    // Pop one or more
    [[nodiscard]]
    std::expected<size_t, sys::error_code>
    async_pop_one_or_more(std::queue<T>& out, Async yield)
    {
        auto slot = _destroy_signal.connect([&] { yield.cancel(); });

        while (_queue.empty()) {
            auto r = _rx_cv.wait(yield);
            if (!r) return std::unexpected(r.error());
        }

        assert(!_queue.empty());

        size_t count = 0;

        while (!_queue.empty()) {
            auto p = std::move(_queue.front());
            _queue.pop_front();
            if (!p.second) {
                out.push(std::move(p.first));
                ++count;
            }
        }

        _tx_cv.notify();

        return count;
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

    asio::any_io_executor get_executor()
    {
        return _ex;
    }

private:
    asio::any_io_executor _ex;
    size_t _max_size;
    Queue _queue;
    ConditionVariable _rx_cv;
    ConditionVariable _tx_cv;
    Cancel _destroy_signal;
};

} // namespace
