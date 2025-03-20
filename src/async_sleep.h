#pragma once

#include <boost/asio/steady_timer.hpp>
#include "namespaces.h"

#include "util/executor.h"
#include "util/signal.h"

namespace ouinet {

using ouinet::util::AsioExecutor;

bool async_sleep( const AsioExecutor& exec
                , asio::steady_timer::duration duration
                , Signal<void()>& cancel
                , asio::yield_context yield);

inline
bool async_sleep( asio::io_context& ctx
                , asio::steady_timer::duration duration
                , Signal<void()>& cancel
                , asio::yield_context yield)
{
    return async_sleep(ctx.get_executor(), duration, cancel, yield);
}

} // ouinet namespace
