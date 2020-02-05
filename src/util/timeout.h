#pragma once

#include "signal.h"
#include "../util/handler_tracker.h"

namespace ouinet { namespace util {

class Timeout {
    struct State {
        asio::steady_timer timer;
        Signal<void()> local_abort_signal;
        bool finished = false;

        State(const asio::executor& ex)
            : timer(ex)
        {}
    };

public:
    template<class Duration>
    Timeout( const asio::executor& ex
           , Signal<void()>& signal
           , Duration duration)
        : _state(std::make_shared<State>(ex))
    {
        _signal_connection = signal.connect([s = _state] {
                if (s->local_abort_signal.call_count() == 0) {
                    s->local_abort_signal();
                }
            });

        asio::spawn(ex, [s = _state, duration] (asio::yield_context yield) {
                TRACK_HANDLER();
                if (s->finished) return;

                sys::error_code ec;

                s->timer.expires_from_now(duration);
                s->timer.async_wait(yield[ec]);

                if (s->finished) return;

                if (s->local_abort_signal.call_count() == 0) {
                    s->local_abort_signal();
                }
            });
    }

    Signal<void()>& abort_signal()
    {
        return _state->local_abort_signal;
    }

    bool timed_out() const
    {
        return _state->local_abort_signal.call_count() != 0;
    }

    ~Timeout()
    {
        _state->finished = true;
        _state->timer.cancel();
    }

private:
    std::shared_ptr<State> _state;
    Signal<void()>::Connection _signal_connection;
};

template<class Duration, class F, class Yield>
auto with_timeout( const asio::executor& ex
                 , Signal<void()>& abort_signal
                 , Duration duration
                 , const F& f
                 , Yield& yield)
{
    Timeout timeout(ex, abort_signal, duration);

    sys::error_code ec;

    auto ret = f(timeout.abort_signal(), yield[ec]);

    if (ec == asio::error::operation_aborted && timeout.timed_out()) {
        ec = asio::error::timed_out;
    }

    return or_throw(yield, ec, std::move(ret));
}

}} // namespaces
