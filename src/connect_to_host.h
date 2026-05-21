#pragma once

#include "namespaces.h"
#include "generic_stream.h"
#include "util/executor.h"

#include <chrono>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/string.hpp>

#include "cxx/dns.h"

namespace ouinet {

class Cancel;
using ouinet::util::AsioExecutor;

asio::ip::tcp::socket
connect_to_host( const AsioExecutor&
               , const std::string& host
               , uint16_t port
               , std::shared_ptr<dns::Resolver> dns_resolver
               , Cancel& cancel_signal
               , asio::yield_context yield);

asio::ip::tcp::socket
connect_to_host( const asio::ip::tcp::resolver::results_type& lookup
               , const AsioExecutor&
               , Cancel& cancel_signal
               , asio::yield_context yield);

asio::ip::tcp::socket
connect_to_host( const asio::ip::tcp::resolver::results_type& lookup
               , const AsioExecutor&
               , std::chrono::steady_clock::duration timeout
               , Cancel& cancel_signal
               , asio::yield_context yield);

} // ouinet namespace
