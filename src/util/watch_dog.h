#pragma once

#include <boost/asio/coroutine.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/spawn.hpp>
#include "../defer.h"
#include "../or_throw.h"
#include "../util/executor.h"
#include "../util/signal.h"
#include "../util/handler_tracker.h"
#include "../namespaces.h"

namespace ouinet {

#include <boost/asio/yield.hpp>

using ouinet::util::AsioExecutor;

template<class OnTimeout>
class NewWatchDog {
private:
    using Clock = std::chrono::steady_clock;

    struct Coro : boost::asio::coroutine {
        NewWatchDog* _wd;

        Coro(const Coro& o) : asio::coroutine(o), _wd(o._wd) {
            if (_wd) _wd->_coro = this;
        }

        Coro(NewWatchDog* wd) : _wd(wd) {
            _wd->_coro = this;
        }

        void operator()(sys::error_code ec = sys::error_code()) {
            if (!_wd) return;
            auto now = Clock::now();

            reenter (this) {
                while (now < _wd->_deadline) {
                    _wd->_timer->expires_after(_wd->_deadline - now);
                    yield _wd->_timer->async_wait(*this);
                }
                auto h = std::move(_wd->_on_timeout);
                // Tell watch dog we're done
                _wd->_coro = nullptr;
                h();
            }
        }
    };

public:
    NewWatchDog() : _coro(nullptr) {}

    NewWatchDog(NewWatchDog&& o)
        : _timer(std::move(o._timer))
        , _deadline(std::move(o._deadline))
        , _on_timeout(std::move(o._on_timeout))
        , _coro(o._coro)
    {
        o._coro = nullptr;
        if (_coro) _coro->_wd = this;
    }

    NewWatchDog& operator=(NewWatchDog&& o)
    {
        _timer = std::move(o._timer);
        _deadline = std::move(o._deadline);
        _on_timeout = std::move(o._on_timeout);
        _coro = o._coro;

        o._coro = nullptr;

        if (_coro) _coro->_wd = this;

        return *this;
    }

    template<class Duration>
    NewWatchDog(const AsioExecutor& ex, Duration d, OnTimeout&& on_timeout)
        : _timer(asio::steady_timer(ex))
        , _deadline(Clock::now() + d)
        , _on_timeout(std::move(on_timeout))
        , _coro(nullptr)
    {
        Coro coro(this);
        coro();
    }

    ~NewWatchDog() {
        // Tell coro we're done
        if (_coro) {
            _coro->_wd = nullptr;
            _timer->cancel();
        }
    }

    bool is_running() const {
        return bool(_coro);
    }

    template<class Duration>
    void expires_after(Duration d)
    {
        assert(_coro);
        if (!_coro) return; // already expired

        auto old_deadline = _deadline;
        _deadline = Clock::now() + d;

        if (_deadline < old_deadline) {
            _timer->cancel();
        }
    }

    Clock::duration time_to_finish() const
    {
        if (!_coro) return Clock::duration(0);
        auto now = Clock::now();
        if (now < _deadline) return _deadline - now;
        return Clock::duration(0);
    }

private:
    boost::optional<asio::steady_timer> _timer;
    Clock::time_point  _deadline;
    OnTimeout _on_timeout;
    Coro* _coro;
};

#include <boost/asio/unyield.hpp>

template<class Duration, class OnTimeout>
inline
NewWatchDog<OnTimeout>
watch_dog(const AsioExecutor& ex, Duration d, OnTimeout&& on_timeout)
{
    return NewWatchDog<OnTimeout>(ex, d, std::move(on_timeout));
}

template<class Duration, class OnTimeout>
inline
NewWatchDog<OnTimeout>
watch_dog(asio::io_context& ctx, Duration d, OnTimeout&& on_timeout)
{
    return NewWatchDog<OnTimeout>(ctx.get_executor(), d, std::move(on_timeout));
}

// Legacy, should eventually be replaced with the above. Problem with this one is
// that it spawn a stackful coroutines which does memory allocation of non trivial
// size. That is problem given that WatchDog is meant to be used quite often.
class WatchDog {
private:
    using Clock = std::chrono::steady_clock;

    struct State {
        WatchDog*          self;
        Clock::time_point  deadline;
        asio::steady_timer timer;

        State(WatchDog* self, Clock::time_point d, const AsioExecutor& ex)
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
    WatchDog(const AsioExecutor& ex, Duration d, OnTimeout on_timeout)
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
    void start(const AsioExecutor& ex, Duration d, OnTimeout on_timeout) {
        stop();

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
        }, asio::detached);
    }

    Clock::duration stop()
    {
        auto ret = time_to_finish();

        if (state) {
            state->timer.cancel();
            state->self = nullptr;
            state = nullptr;
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

// This yields a timeout
// if the given watch dog expired
// (unless the `cancel` signal did fire).
//
// Please note that you should not pass as `cancel`
// a derived signal used to report the watch dog timeout,
// but its parent instead,
// so that timeouts can be distinguished from cancellations.
template<class WDog>
inline
sys::error_code
compute_error_code( const sys::error_code& ec
                  , const Cancel& cancel
                  , const WDog& watch_dog)
{
    sys::error_code ec_ = compute_error_code(ec, cancel);
    if (ec_ == asio::error::operation_aborted) return ec_;
    if (!watch_dog.is_running()) return asio::error::timed_out;
    return ec;
}

// This is similar to `return_or_throw_on_error`,
// but it also yields a timeout
// if the given watch dog expired
// (unless the `cancel` signal did fire).
//
// Please note that you should not pass as `cancel`
// a derived signal used to report the watch dog timeout,
// but its parent instead,
// so that timeouts can be distinguished from cancellations.
#define fail_on_error_or_timeout(yield, cancel, ec, watch_dog, ...) { \
    sys::error_code ec_ = compute_error_code(ec, cancel, watch_dog); \
    if (ec_) return or_throw(yield, ec_, ##__VA_ARGS__); \
}

} // namespace
