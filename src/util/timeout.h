#pragma once

#include "cancel.h"
#include "../task.h"

namespace ouinet { namespace util {

class Timeout {
    struct State {
        asio::steady_timer timer;
        Cancel local_abort_signal;
        bool finished = false;

        State(const AsioExecutor& ex)
            : timer(ex)
        {}
    };

public:
    template<class Duration>
    Timeout( const AsioExecutor& ex
           , Cancel& signal
           , Duration duration)
        : _state(std::make_shared<State>(ex))
    {
        _signal_connection = signal.connect([s = _state] {
                if (!s->local_abort_signal) {
                    s->local_abort_signal();
                }
            });

        task::spawn_detached(ex, [s = _state, duration] (asio::yield_context yield) {
                if (s->finished) return;

                sys::error_code ec;

                s->timer.expires_after(duration);
                s->timer.async_wait(yield[ec]);

                if (s->finished) return;

                if (!s->local_abort_signal) {
                    s->local_abort_signal();
                }
            });
    }

    Cancel& abort_signal()
    {
        return _state->local_abort_signal;
    }

    bool timed_out() const
    {
        return (bool) _state->local_abort_signal;
    }

    ~Timeout()
    {
        _state->finished = true;
        _state->timer.cancel();
    }

private:
    std::shared_ptr<State> _state;
    Cancel::Connection _signal_connection;
};

template<class Duration, class F, class Yield>
auto with_timeout( const AsioExecutor& ex
                 , Cancel& abort_signal
                 , Duration duration
                 , const F& f
                 , Yield& yield)
{
    Timeout timeout(ex, abort_signal, duration);

    sys::error_code ec;

    auto ret = f(timeout.abort_signal(), yield[ec]);

    if (!abort_signal && ec == asio::error::operation_aborted && timeout.timed_out()) {
        ec = asio::error::timed_out;
    }
    // If `abort_signal` itself was triggered,
    // keep the error from `f` (probably `asio::error::operation_aborted`).

    return or_throw(yield, ec, std::move(ret));
}

}} // namespaces
