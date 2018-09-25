#pragma once

namespace ouinet { namespace default_timeout {

static inline auto tcp_connect() { return std::chrono::minutes(4); }
static inline auto fetch_http() { return std::chrono::minutes(8); }

}} // namespaces
