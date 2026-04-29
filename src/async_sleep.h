#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/spawn.hpp>
#include "namespaces.h"
#include "util/signal.h"

namespace ouinet {

class Async;

// Returns `false` if cancelled
bool async_sleep( asio::steady_timer::duration duration
                , Cancel& cancel
                , asio::yield_context yield);

// Throws `Async::Cancelled` if cancelled
void async_sleep(asio::steady_timer::duration duration, Async);

} // ouinet namespace
