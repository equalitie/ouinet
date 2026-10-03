#pragma once

#include "util/cancel.h"

#include "api.h"
#include "namespaces.h"
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

namespace ouinet {

class Async;

// Returns `false` if cancelled
OUINET_COMMON_API
bool async_sleep(asio::steady_timer::duration duration, Cancel &cancel,
                 asio::yield_context yield);

// Throws `Async::Cancelled` if cancelled
OUINET_COMMON_API
void async_sleep(asio::steady_timer::duration duration, Async);

} // namespace ouinet
