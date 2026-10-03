#pragma once

#include "namespaces.h"
#include "util/crypto_stream.h"
#include <boost/beast/http/message.hpp>
#include <optional>
#include <string_view>

namespace ouinet::cache::resource_key {

CryptoStreamKey from_url(std::string_view url);
std::optional<CryptoStreamKey>
from_cached_header(http::response_header<> const &hdr);

} // namespace ouinet::cache::resource_key
