#pragma once

#include "../defer.h"

namespace ouinet {

class WatchDog {
private:
    using Clock = std::chrono::steady_clock;

public:
    WatchDog(const WatchDog&) = delete;

    WatchDog(WatchDog&& other)
        : _deadline(other._deadline)
        , _self_in_lambda(other._self_in_lambda)
        , _timer(other._timer)
    {
        other._deadline = nullptr;
        other._self_in_lambda = nullptr;
        other._timer = nullptr;

        *_self_in_lambda = this;
    }

    WatchDog& operator=(WatchDog&& other)
    {
        _deadline = other._deadline;
        _self_in_lambda = other._self_in_lambda;
        _timer = other._timer;

        *_self_in_lambda = this;

        other._deadline = nullptr;
        other._self_in_lambda = nullptr;
        other._timer = nullptr;

        return *this;
    }

    template<class Duration, class OnTimeout>
    WatchDog(asio::io_service& ios, Duration d, OnTimeout on_timeout)
    {
        asio::spawn(ios, [self_ = this, &ios, d, on_timeout = std::move(on_timeout)]
                         (asio::yield_context yield) mutable {
            Clock::time_point deadline = Clock::now() + d;
            WatchDog* self = self_;

            asio::steady_timer timer(ios);

            self->_self_in_lambda = &self;
            self->_timer          = &timer;
            self->_deadline       = &deadline;

            {
                auto on_exit = defer([&] {
                        if (!self) return;
                        self->_self_in_lambda = nullptr;
                        self->_timer = nullptr;
                        self->_deadline = nullptr;
                    });

                auto now = Clock::now();
                while (now < deadline) {
                    timer.expires_after(deadline - now);
                    sys::error_code ec;
                    timer.async_wait(yield[ec]);
                    if (!self) return;
                    now = Clock::now();
                }
            }

            on_timeout();
        });
    }

    template<class Duration>
    void expires_after(Duration d)
    {
        if (!_deadline) return;
        auto old_deadline = *_deadline;
        *_deadline = Clock::now() + d;

        if (*_deadline < old_deadline) {
            assert(_timer);
            _timer->cancel();
        }
    }

    ~WatchDog() {
        if (_self_in_lambda) {
            _timer->cancel();
            *_self_in_lambda = nullptr;
        }
    }

private:
    Clock::time_point* _deadline;
    WatchDog** _self_in_lambda = nullptr;
    asio::steady_timer* _timer;
};


} // namespace
