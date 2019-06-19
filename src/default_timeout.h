#pragma once

#include <chrono>

namespace ouinet { namespace default_timeout {

static inline auto tcp_connect() { return std::chrono::minutes(4); }
static inline auto fetch_http() { return std::chrono::minutes(8); }
static inline auto http_forward() { return std::chrono::seconds(60); }

}} // namespaces
