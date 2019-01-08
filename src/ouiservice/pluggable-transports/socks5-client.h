#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>

#include "../../namespaces.h"
#include "../../util/signal.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

/*
 * Connects to $destination_endpoint using a socks5 proxy at $proxy_endpoint.
 * Supports optional connection arguments key/value pairs,
 * communicated via socks5 authentication.
 * On success, returns a TCP socket that is ready for payload data.
 */
asio::ip::tcp::socket connect_socks5(
    asio::ip::tcp::endpoint proxy_endpoint,
    asio::ip::tcp::endpoint destination_endpoint,
    boost::optional<std::map<std::string, std::string>> connection_arguments,
    asio::io_service& ios,
    asio::yield_context yield,
    Signal<void()>& cancel
);

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
