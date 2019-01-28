#pragma once

#include "../defer.h"

namespace ouinet {

class WatchDog {
private:
    using Clock = std::chrono::steady_clock;

public:
    template<class Duration, class OnTimeout>
    WatchDog(asio::io_service& ios, Duration d, OnTimeout on_timeout)
    {
        asio::spawn(ios, [&, d, on_timeout = std::move(on_timeout)]
                         (asio::yield_context yield) {
            Clock::time_point deadline = Clock::now() + d;
            bool was_destroyed         = false;

            asio::steady_timer timer(ios);

            _was_destroyed = &was_destroyed;
            _timer         = &timer;
            _deadline      = &deadline;

            {
                auto on_exit = defer([&] {
                        if (was_destroyed) return;
                        _was_destroyed = nullptr;
                        _timer = nullptr;
                        _deadline = nullptr;
                    });

                auto now = Clock::now();
                while (now < deadline) {
                    timer.expires_after(deadline - now);
                    sys::error_code ec;
                    timer.async_wait(yield[ec]);
                    if (was_destroyed) return;
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
        if (_was_destroyed) *_was_destroyed = true;
        if (_timer) _timer->cancel();
    }

private:
    Clock::time_point* _deadline;
    bool* _was_destroyed = nullptr;
    asio::steady_timer* _timer;
};


} // namespace
