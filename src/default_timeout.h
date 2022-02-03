#pragma once

#include <chrono>

namespace ouinet { namespace default_timeout {

// Ongoing data traffic in a connection.
static inline auto activity() { return std::chrono::minutes(3); }

static inline auto tcp_connect() { return std::chrono::minutes(4); }

// Sending an HTTP message over an existing connection,
// either without body (just the head) or a small one.
static inline auto http_send_simple() { return std::chrono::seconds(60); }

// Receiving an HTTP message over an existing connection,
// either without body (just the head) or a small one.
static inline auto http_recv_simple() { return std::chrono::seconds(55); }

// Same thing, but for the first message in an incoming connection
// (so that accidental connections get closed fast).
static inline auto http_recv_simple_first() { return std::chrono::seconds(5); }

// The whole operation of looking up a host, connecting to it,
// sending an HTTP request, and getting the response head
// (not including the body).
static inline auto fetch_http() { return std::chrono::minutes(8); }

}} // namespaces
