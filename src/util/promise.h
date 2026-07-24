#pragma once

#include "util/async.h"
#include "util/wait_condition.h"
#include "util/overloaded.h"
#include "error.h"
#include <variant>

namespace ouinet {

template<class Value> class Promise {
public:
    // Error for when promise was destroyed without calling `set_value`.
    struct BrokenPromise {
        operator sys::error_code() const {
            return OuinetError::broken_promise;
        }
    };

private:
    struct Shared {
        struct Waiting {
            WaitCondition wc;
            WaitCondition::Lock lock;

            Waiting(asio::any_io_executor exec):
                wc(std::move(exec)),
                lock(wc.lock())
            {}
        };

        Shared(asio::any_io_executor exec):
            state(Waiting(std::move(exec))),
            promise_counter(1)
        {}

        Shared(Value value):
            state(std::move(value)),
            promise_counter(0)
        {}

        std::variant<Waiting, Value> state;
        size_t promise_counter;
    };

public:
    class Future {
    public:
        std::expected<Value, BrokenPromise> wait(Async yield) {
            using R = std::expected<Value, BrokenPromise>;

            return std::visit( overloaded {
                    [&] (Shared::Waiting& s) -> R {
                        s.wc.wait(yield);
                        auto vp = std::get_if<Value>(&_shared->state);
                        if (!vp) return std::unexpected(BrokenPromise());
                        return *vp;
                    },
                    [] (const Value& v) -> R {
                        return v;
                    }
                },
                _shared->state);
        }

        static Future make_ready(Value v) {
            auto shared = std::make_shared<Shared>(std::move(v));
            shared->promise_counter = 0;
            return Future(std::move(shared));
        }

    private:
        template<class> friend class Promise;

        Future(std::shared_ptr<Shared> shared)
            : _shared(std::move(shared))
        {}

    private:
        std::shared_ptr<Shared> _shared;
    };

    Future get_future() {
        return Future { _shared };
    }

    void set_value(Value value) {
        _shared->state.template emplace<Value>(std::move(value));
    }

    Promise(asio::any_io_executor exec):
        _shared(std::make_shared<Shared>(exec))
    {}

    Promise(const Promise& other) {
        *this = other;
    }

    Promise& operator=(const Promise& other) {
        if (_shared && _shared != other._shared) {
            if (--_shared->promise_counter == 0) {
                if (auto w = std::get_if<typename Shared::Waiting>(&_shared->state)) {
                    w->lock.release();
                }
            }
        }

        _shared = other._shared;

        if (_shared) {
            _shared->promise_counter += 1;
        }

        return *this;
    }

    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;

    ~Promise() {
        if (!_shared) return;
        if (--_shared->promise_counter == 0) {
            if (auto w = std::get_if<typename Shared::Waiting>(&_shared->state)) {
                w->lock.release();
            }
        }
    }

private:
    std::shared_ptr<Shared> _shared;
};

} // namespace
