#include "async_sleep.h"

namespace ouinet {

bool async_sleep( const AsioExecutor& exec
                , asio::steady_timer::duration duration
                , Signal<void()>& cancel
                , asio::yield_context yield)
{
    if (cancel) {
        return false;
    }

    asio::steady_timer timer(exec);
    timer.expires_after(duration);
    sys::error_code ec;

    auto stop_timer = cancel.connect([&timer] {
        timer.cancel();
    });

    timer.async_wait(yield[ec]);

    if (ec || cancel) {
        return false;
    }

    return true;
}

} // namespace
