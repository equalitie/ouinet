#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>
#include <concepts>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

namespace ouinet {
namespace ouisync_service {
namespace detail {

// HACK: `ouinet::ConditionVariable` currently has a bug which causes it to not work with callbacks
// as completion tokens. Using this class as a replacement until that bug is fixed.
class ConditionVariable {
public:
    ConditionVariable(const boost::asio::any_io_executor& exec)
        : _exec(exec)
    {}

    const boost::asio::any_io_executor& get_executor() {
        return _exec;
    }

    template<typename CompletionToken>
    requires boost::asio::completion_token_for<CompletionToken, void(boost::system::error_code)>
    auto wait(CompletionToken token) {
        return boost::asio::async_initiate<
            CompletionToken,
            void(boost::system::error_code)
        > (
            [this] (auto handler) {
                _waiters.push_back(std::move(handler));
            },
            token
        );
    }

    void notify(boost::system::error_code ec = {}) {
        auto waiters = std::move(_waiters);
        for (auto& waiter : waiters) {
            std::move(waiter)(ec);
        }
    }

private:
    boost::asio::any_io_executor _exec;
    std::vector<boost::asio::any_completion_handler<void(boost::system::error_code)>> _waiters;
};

// Async queue that serves as intermediate buffer for sending and receiving UDP datagrams.
//
// NOTE: this is similar to `async_queue` but it's currently not generic, has simpler API and
// supports generic completion tokens.
// TODO: Modify `async_queue` to support generic completion tokens and use that instead.
class Queue {
public:
    using value_type = std::tuple<
        boost::asio::ip::udp::endpoint,
        std::vector<uint8_t>
    >;

    using message_type = std::tuple<
        boost::system::error_code,
        boost::asio::ip::udp::endpoint,
        std::vector<uint8_t>
    >;

public:

    Queue(
        const boost::asio::any_io_executor& exec,
        size_t capacity = std::numeric_limits<size_t>::max()
    )
        : _capacity(capacity)
        , _push_cv(exec)
        , _pop_cv(exec)
    {}

    ~Queue() {
        cancel();
    }

    const boost::asio::any_io_executor& get_executor() {
        return _push_cv.get_executor();
    }

    template<typename CompletionToken>
    requires boost::asio::completion_token_for<
        CompletionToken,
        void(boost::system::error_code)
    >
    auto async_push(
        boost::system::error_code ec,
        value_type value,
        CompletionToken token
    ) {
        return boost::asio::async_initiate<
            CompletionToken,
            void(boost::system::error_code)
        >(
            [this, ec, value = std::move(value)] (auto handler) {
                wait(
                    _pop_cv, [this] { return !full(); },
                    [this, ec, value = std::move(value), handler = std::move(handler)]
                    (boost::system::error_code wait_ec) mutable {
                        if (!wait_ec) {
                            push(ec, std::move(value));
                        }

                        handler(wait_ec);
                    }
                );
            },
            token
        );
    }

    template<typename CompletionToken>
    requires boost::asio::completion_token_for<
        CompletionToken,
        void(boost::system::error_code, value_type)
    >
    auto async_pop(CompletionToken token) {
        return boost::asio::async_initiate<
            CompletionToken,
            void(boost::system::error_code, value_type)
        >(
            [this] (auto handler) {
                wait(
                    _push_cv,
                    [this] { return !empty(); },
                    [this, handler = std::move(handler)]
                    (boost::system::error_code wait_ec) mutable {
                        if (!wait_ec) {
                            auto [ec, ep, data] = pop();

                            handler(
                                ec,
                                std::make_tuple(ep, std::move(data))
                            );
                        } else {
                            handler(
                                wait_ec,
                                std::make_tuple(boost::asio::ip::udp::endpoint(), std::vector<uint8_t>())
                            );
                        }
                    }
                );
            },
            token
        );
    }

    bool try_push(boost::system::error_code ec, value_type value) {
        if (full()) {
            return false;
        }

        push(ec, std::move(value));

        return true;
    }

    std::optional<message_type> try_pop() {
        if (empty()) {
            return std::nullopt;
        }

        return pop();
    }

    // Cancels ongoing `async_push` and `async_pop`. Does not modify the content of the queue.
    void cancel() {
        _push_cv.notify(boost::asio::error::operation_aborted);
        _pop_cv.notify(boost::asio::error::operation_aborted);
    }

    // Total number of bytes in this queue
    size_t bytes() const {
        return std::accumulate(
            _queue.begin(),
            _queue.end(),
            0,
            [] (size_t sum, const message_type& e) {
                return sum + std::get<2>(e).size();
            }
        );
    }

    size_t size() const {
        return _queue.size();
    }

    bool empty() const {
        return _queue.empty();
    }

    bool full() const {
        return _queue.size() >= _capacity;
    }

private:

    void push(boost::system::error_code ec, value_type value) {
        assert(!full());

        auto [ ep, data ] = std::move(value);
        _queue.push_back({ ec, ep, std::move(data) });
        _push_cv.notify();
    }

    message_type pop() {
        assert(!empty());

        auto message = std::move(_queue.front());
        _queue.pop_front();
        _pop_cv.notify();

        return message;
    }

    // Wait on the condition variable until the predicate returns true.
    template<typename Predicate, typename CompletionHandler>
    requires
        std::predicate<Predicate> &&
        std::invocable<CompletionHandler, boost::system::error_code>
    static void wait(ConditionVariable& cv, Predicate predicate, CompletionHandler handler) {
        if (predicate()) {
            handler(boost::system::error_code());
            return;
        }

        cv.wait(
            [
                &cv,
                predicate = std::forward<Predicate>(predicate),
                handler = std::forward<CompletionHandler>(handler)
            ]
            (boost::system::error_code ec) mutable {
                if (ec) {
                    handler(ec);
                    return;
                }

                wait(
                    cv,
                    std::forward<Predicate>(predicate),
                    std::forward<CompletionHandler>(handler)
                );
            }
        );
    }

private:
    std::deque<message_type> _queue;
    size_t _capacity;
    ConditionVariable _push_cv;
    ConditionVariable _pop_cv;
};

}}} // namespace ouinet::ouisync_service::detail
