#pragma once

#include "../defer.h"

namespace ouinet {

class WatchDog {
private:
    using Clock = std::chrono::steady_clock;

    struct State {
        WatchDog*          self;
        Clock::time_point  deadline;
        asio::steady_timer timer;

        State(WatchDog* self, Clock::time_point d, asio::io_service& ios)
            : self(self)
            , deadline(d)
            , timer(ios)
        {}
    };

public:
    WatchDog() : state(nullptr) {}

    WatchDog(const WatchDog&) = delete;

    WatchDog(WatchDog&& other)
        : state(other.state)
    {
        other.state = nullptr;
        if (state) state->self = this;
    }

    WatchDog& operator=(WatchDog&& other)
    {
        stop();

        state = other.state;
        other.state = nullptr;
        if (state) state->self = this;
        return *this;
    }

    template<class Duration, class OnTimeout>
    WatchDog(asio::io_service& ios, Duration d, OnTimeout on_timeout)
    {
        start(ios, d, std::move(on_timeout));
    }

    template<class Duration>
    void expires_after(Duration d)
    {
        if (!state) return;
        auto old_deadline = state->deadline;
        state->deadline = Clock::now() + d;

        if (state->deadline < old_deadline) {
            state->timer.cancel();
        }
    }

    void expires_at(Clock::time_point t)
    {
        if (!state) return;
        auto old_deadline = state->deadline;
        state->deadline = t;

        if (state->deadline < old_deadline) {
            state->timer.cancel();
        }
    }

    bool is_running() const {
        return bool(state);
    }

    Clock::duration pause() {
        auto ret = time_to_finish();
        expires_at(Clock::time_point::max());
        return ret;
    }

    ~WatchDog() {
        stop();
    }

    template<class Duration, class OnTimeout>
    void start(asio::io_service& ios, Duration d, OnTimeout on_timeout) {
        stop();

        asio::spawn(ios, [self_ = this, &ios, d, on_timeout = std::move(on_timeout)]
                         (asio::yield_context yield) mutable {
            State state(self_, Clock::now() + d, ios);
            self_->state = &state;

            {
                auto on_exit = defer([&] {
                        if (!state.self) return;
                        state.self->state = nullptr;
                    });

                auto now = Clock::now();
                while (now < state.deadline) {
                    state.timer.expires_after(state.deadline - now);
                    sys::error_code ec;
                    state.timer.async_wait(yield[ec]);
                    if (!state.self) return;
                    now = Clock::now();
                }
            }

            on_timeout();
        });
    }

    Clock::duration stop()
    {
        auto ret = time_to_finish();

        if (state) {
            state->timer.cancel();
            state->self = nullptr;
        }

        return ret;
    }

    Clock::duration time_to_finish() const
    {
        if (!state) return Clock::duration(0);

        auto end = state->deadline;
        auto now = Clock::now();

        if (now < end) return end - now;
        return Clock::duration(0);
    }

private:
    State* state = nullptr;
};


} // namespace
