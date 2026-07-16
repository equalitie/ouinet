#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>
#include <concepts>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>

namespace ouinet {
namespace ouisync_service {
namespace detail {

// HACK: `ouinet::ConditionVariable` currently has a bug which causes it to not work with callbacks
// as completion tokens. Using this class as a replacement until that bug is fixed.
class ConditionVariable {
    using Handler = boost::asio::any_completion_handler<void(boost::system::error_code)>;

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
            [this] (auto handler) mutable {
                auto cancellation_slot = boost::asio::get_associated_cancellation_slot(handler);
                auto id = insert_handler(std::move(handler));

                if (cancellation_slot.is_connected()) {
                    cancellation_slot.assign(
                        [this, id] (boost::asio::cancellation_type type) {
                            invoke(take_handler(id), boost::asio::error::operation_aborted);
                        }
                    );
                }
            },
            token
        );
    }

    void notify(boost::system::error_code ec = {}) {
        for (auto& p : _handlers) {
            invoke(std::move(p.second), ec);
        }

        _handlers.clear();
    }

private:
    size_t insert_handler(Handler&& handler) {
        auto id = _next_handler_id++;
        _handlers.insert({ id, std::move(handler) });
        return id;
    }

    Handler take_handler(size_t id) {
        auto it = _handlers.find(id);
        if (it != _handlers.end()) {
            auto handler = std::move(it->second);
            _handlers.erase(it);
            return handler;
        } else {
            return Handler();
        }
    }

    void invoke(Handler&& handler, boost::system::error_code ec) {
        if (handler) {
            boost::asio::post(_exec, [handler = std::move(handler), ec] mutable {
               handler(ec);
            });
        }
    }

private:
    boost::asio::any_io_executor _exec;
    size_t _next_handler_id = 0;
    std::unordered_map<size_t, Handler> _handlers;
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
            [this] (auto handler) mutable {
                auto cancellation_slot = boost::asio::get_associated_cancellation_slot(handler);

                wait(
                    _push_cv,
                    [this] { return !empty(); },
                    boost::asio::bind_cancellation_slot(
                        std::move(cancellation_slot),
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
                                    std::make_tuple(
                                        boost::asio::ip::udp::endpoint(),
                                        std::vector<uint8_t>()
                                    )
                                );
                            }
                        }
                    )
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
    template<typename Predicate, typename CompletionToken>
    requires
        std::predicate<Predicate> &&
        boost::asio::completion_token_for<CompletionToken, void(boost::system::error_code)>
    static auto wait(ConditionVariable& cv, Predicate predicate, CompletionToken token) {
        return boost::asio::async_compose<CompletionToken, void(boost::system::error_code)>(
            [
                &cv,
                predicate = std::forward<Predicate>(predicate)
            ]
            (auto& self, boost::system::error_code ec = {}) {
                self.reset_cancellation_state(boost::asio::enable_total_cancellation());

                if (ec) {
                    self.complete(ec);
                    return;
                }

                if (predicate()) {
                    self.complete(boost::system::error_code());
                    return;
                }

                cv.wait(std::move(self));
            },
            token
        );
    }

private:
    std::deque<message_type> _queue;
    size_t _capacity;
    ConditionVariable _push_cv;
    ConditionVariable _pop_cv;
};

}}} // namespace ouinet::ouisync_service::detail
