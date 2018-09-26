#pragma once

namespace ouinet { namespace default_timeout {

static inline auto tcp_connect() { return std::chrono::minutes(2); }
static inline auto fetch_http() { return std::chrono::minutes(3); }

}} // namespaces
