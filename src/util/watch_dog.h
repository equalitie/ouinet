#pragma once

#include "../defer.h"
#include "../util/handler_tracker.h"

namespace ouinet {

class WatchDog {
private:
    using Clock = std::chrono::steady_clock;

    struct State {
        WatchDog*          self;
        Clock::time_point  deadline;
        asio::steady_timer timer;

        State(WatchDog* self, Clock::time_point d, const asio::executor& ex)
            : self(self)
            , deadline(d)
            , timer(ex)
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
    WatchDog(const asio::executor& ex, Duration d, OnTimeout on_timeout)
    {
        start(ex, d, std::move(on_timeout));
    }

    template<class Duration, class OnTimeout>
    WatchDog(asio::io_context& ctx, Duration d, OnTimeout on_timeout)
        : WatchDog(ctx.get_executor(), d, std::move(on_timeout))
    {}

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
    void start(const asio::executor& ex, Duration d, OnTimeout on_timeout) {
        stop();

        // This is defined in boost/libs/coroutine/src/{posix,windows}/stack_traits.cpp
        // It seems to be defined to an arbitrary number 8*1024. I.e. it
        // doesn't seem to be derived from sizes of some internal structures of
        // the coroutine library.
        size_t min_coro_size = boost::coroutines::stack_traits::minimum_size();

        boost::coroutines::attributes attribs;
        attribs.size = min_coro_size;

        asio::spawn(ex, [self_ = this, ex, d, on_timeout = std::move(on_timeout)]
                         (asio::yield_context yield) mutable {
            TRACK_HANDLER();
            State state(self_, Clock::now() + d, ex);
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
        }, attribs);
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
