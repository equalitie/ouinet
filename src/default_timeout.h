#pragma once

#include <chrono>

namespace ouinet { namespace default_timeout {

// Ongoing data traffic in a connection.
static inline auto activity() { return std::chrono::minutes(3); }

static inline auto tcp_connect() { return std::chrono::minutes(4); }
static inline auto fetch_http() { return std::chrono::minutes(8); }
static inline auto http_request() { return std::chrono::seconds(60); }

}} // namespaces
