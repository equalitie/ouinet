#include "async_sleep.h"
#include "util/async.h"
#include "util/cancel.h"

namespace ouinet {

bool async_sleep( asio::steady_timer::duration duration
                , Cancel& cancel
                , asio::yield_context yield)
{
    if (cancel) {
        return false;
    }

    asio::steady_timer timer(yield.get_executor());
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

void async_sleep(asio::steady_timer::duration duration, Async yield)
{
    if (yield.is_cancelled()) {
        throw Async::Cancelled();
    }

    asio::steady_timer timer(yield.get_executor());
    timer.expires_after(duration);

    auto stop_timer = yield.cancel_slot([&timer] {
        timer.cancel();
    });

    auto r = timer.async_wait(yield);

    if (!r) {
        assert(r.error() == asio::error::operation_aborted);
        throw Async::Cancelled();
    }
}

} // namespace
