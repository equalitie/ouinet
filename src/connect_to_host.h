#pragma once

#include "namespaces.h"
#include "generic_stream.h"
#include "util/executor.h"

#include <chrono>
#include <expected>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/string.hpp>

#include "cxx/dns.h"
#include "api.h"

namespace ouinet {

class Async;
class Cancel;
using ouinet::util::AsioExecutor;

OUINET_COMMON_API
[[nodiscard]]
std::expected<asio::ip::tcp::socket, sys::error_code>
connect_to_host( const std::string& host
               , uint16_t port
               , std::shared_ptr<dns::Resolver> dns_resolver
               , Async yield);


OUINET_COMMON_API
[[nodiscard]]
std::expected<asio::ip::tcp::socket, sys::error_code>
connect_to_host( const asio::ip::tcp::resolver::results_type& lookup, Async);

OUINET_COMMON_API
[[nodiscard]]
std::expected<asio::ip::tcp::socket, sys::error_code>
connect_to_host( const asio::ip::tcp::resolver::results_type& lookup
               , std::chrono::steady_clock::duration timeout
               , Async yield);

} // ouinet namespace
