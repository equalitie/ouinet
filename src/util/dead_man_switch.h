#pragma once

namespace ouinet {

class DeadManSwitch {
public:
    template<class Duration, class OnTimeout>
    DeadManSwitch(asio::io_service& ios, Duration d, OnTimeout on_timeout)
        : _ios(ios)
    {
        asio::spawn(ios, [&, d, on_timeout = std::move(on_timeout)]
                         (asio::yield_context yield) {
            bool was_destroyed = false;
            asio::steady_timer timer(ios);

            _was_destroyed = &was_destroyed;
            _timer = &timer;

            auto on_exit = defer([&] {
                    _was_destroyed = nullptr;
                    _timer = nullptr;
                });

            timer.expires_from_now(d);
            sys::error_code ec;
            timer.async_wait(yield[ec]);

            if (was_destroyed) return;

            on_timeout();
        });
    }

    ~DeadManSwitch() {
        if (_was_destroyed) *_was_destroyed = true;
        if (_timer) _timer->cancel();
    }

private:
    asio::io_service& _ios;
    bool* _was_destroyed = nullptr;
    asio::steady_timer* _timer;
};


} // namespace
