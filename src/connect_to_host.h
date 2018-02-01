#pragma once

#include "namespaces.h"
#include "generic_connection.h"

#include <boost/beast/core/string.hpp>

namespace ouinet {

GenericConnection
connect_to_host( asio::io_service& ios
               , beast::string_view host_and_port
               , sys::error_code& ec
               , asio::yield_context yield);

} // ouinet namespace
