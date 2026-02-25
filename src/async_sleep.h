#pragma once

#include <boost/asio/steady_timer.hpp>
#include "namespaces.h"
#include "util/signal.h"

namespace ouinet {

bool async_sleep( asio::steady_timer::duration duration
                , Cancel& cancel
                , asio::yield_context yield);

} // ouinet namespace
