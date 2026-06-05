#pragma once

#include "util/async.h"
#include "util/wait_condition.h"
#include <optional>

namespace ouinet {

template<class Value> class Promise {
private:
    struct State {
        State(asio::any_io_executor exec):
            wc(std::move(exec)),
            lock(wc.lock())
        {}

        WaitCondition wc;
        WaitCondition::Lock lock;
        std::optional<Value> value;
        size_t promise_counter = 1;
    };

public:
    // Error for when promise was destroyed without calling `set_value`.
    struct BrokenPromise {};

    class Future {
    public:
        std::expected<std::reference_wrapper<Value>, BrokenPromise> wait(Async yield) {
            _state->wc.wait(yield);
            if (!_state->value) return std::unexpected(BrokenPromise());
            return *_state->value;
        }

    private:
        template<class> friend class Promise;

        Future(std::shared_ptr<State> state)
            : _state(std::move(state))
        {}

    private:
        std::shared_ptr<State> _state;
    };

    Future get_future() {
        return Future { _state };
    }

    void set_value(Value value) {
        _state->value = std::move(value);
        _state->lock.release();
    }

    Promise(asio::any_io_executor exec):
        _state(std::make_shared<State>(exec))
    {}

    Promise(const Promise& other) {
        *this = other;
    }

    Promise& operator=(const Promise& other) {
        if (_state && _state != other._state) {
            if (--_state->promise_counter == 0) {
                _state->lock.release();
            }
        }

        _state = other._state;

        if (_state) {
            _state->promise_counter += 1;
        }

        return *this;
    }

    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;

    ~Promise() {
        if (!_state) return;
        if (--_state->promise_counter == 0) {
            _state->lock.release();
        }
    }

private:
    std::shared_ptr<State> _state;
};

} // namespace
