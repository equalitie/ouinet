#pragma once

#include <string_view>
#include <optional>
#include <boost/beast/http/message.hpp>
#include "util/crypto_stream.h"
#include "namespaces.h"

namespace ouinet::cache::resource_key {

CryptoStreamKey from(std::string_view url);
std::optional<CryptoStreamKey> from(http::response_header<> const& hdr);

} // namespace
