#pragma once

#include <chrono>

namespace ouinet { namespace default_timeout {

// Ongoing data traffic in a connection.
static inline auto activity() { return std::chrono::minutes(3); }

static inline auto tcp_connect() { return std::chrono::minutes(4); }

// Sending an HTTP message over an existing connection,
// either without body (just the head) or a small one.
static inline auto http_send_simple() { return std::chrono::seconds(60); }

static inline auto fetch_http() { return std::chrono::minutes(8); }

}} // namespaces
