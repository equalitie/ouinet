#pragma once

#include <string_view>
#include <optional>
#include <boost/beast/http/message.hpp>
#include "util/crypto_stream.h"
#include "namespaces.h"

namespace ouinet::cache::resource_key {

CryptoStreamKey from_url(std::string_view url);
std::optional<CryptoStreamKey> from_cached_header(http::response_header<> const& hdr);

} // namespace
