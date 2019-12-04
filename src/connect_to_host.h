#pragma once

#include "namespaces.h"
#include "generic_stream.h"
#include "or_throw.h"
#include "util/signal.h"

#include <chrono>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/string.hpp>


namespace ouinet {

asio::ip::tcp::socket
connect_to_host( asio::executor&
               , const std::string& host
               , const std::string& port
               , Signal<void()>& cancel_signal
               , asio::yield_context yield);

asio::ip::tcp::socket
connect_to_host( const asio::ip::tcp::resolver::results_type& lookup
               , asio::executor&
               , Signal<void()>& cancel_signal
               , asio::yield_context yield);

asio::ip::tcp::socket
connect_to_host( const asio::ip::tcp::resolver::results_type& lookup
               , asio::executor&
               , std::chrono::steady_clock::duration timeout
               , Signal<void()>& cancel_signal
               , asio::yield_context yield);

} // ouinet namespace
