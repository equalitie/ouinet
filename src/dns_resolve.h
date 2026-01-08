#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include "util/signal.h"
#include "util/yield.h"

namespace ouinet {

using TcpLookup = asio::ip::tcp::resolver::results_type;

TcpLookup
resolve_target( const http::request_header<>& req
              , bool allow_private_targets
              , boost::asio::any_io_executor exec
              , Cancel& cancel
              , YieldContext yield);

} // namespace
