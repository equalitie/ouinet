#pragma once

#include <boost/asio/steady_timer.hpp>
#include "namespaces.h"

#include "util/signal.h"

namespace ouinet {

inline
bool async_sleep( const AsioExecutor& exec
                , asio::steady_timer::duration duration
                , Signal<void()>& cancel
                , asio::yield_context yield)
{
    asio::steady_timer timer(exec);
    timer.expires_from_now(duration);
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

inline
bool async_sleep( asio::io_context& ctx
                , asio::steady_timer::duration duration
                , Signal<void()>& cancel
                , asio::yield_context yield)
{
    asio::steady_timer timer(ctx);
    timer.expires_from_now(duration);
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

} // ouinet namespace
