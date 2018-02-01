#pragma once

#include <boost/asio/steady_timer.hpp>
#include "namespaces.h"

namespace ouinet {

inline
void async_sleep( asio::io_service& ios
                , asio::steady_timer::duration duration
                , asio::yield_context yield)
{
    asio::steady_timer timer(ios);
    timer.expires_from_now(duration);
    sys::error_code ec;
    timer.async_wait(yield[ec]);
}

} // ouinet namespace
