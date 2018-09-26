#pragma once

#include "namespaces.h"
#include "generic_connection.h"
#include "or_throw.h"
#include "util/signal.h"

#include <chrono>
#include <boost/beast/core/string.hpp>

namespace ouinet {

GenericConnection
connect_to_host( asio::io_service& ios
               , const std::string& host
               , const std::string& port
               , std::chrono::steady_clock::duration timeout
               , Signal<void()>& cancel_signal
               , asio::yield_context yield);

} // ouinet namespace
