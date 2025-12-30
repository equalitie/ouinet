#include "resource_key.h"
#include "util/hash.h"
#include "constants.h"

namespace ouinet::cache::resource_key {

static const std::string_view SALT = "ouinet-resource-key-salt";

CryptoStreamKey from_url(std::string_view url) {
    util::SHA256 hash;
    hash.update(SALT);
    hash.update(url);
    return CryptoStreamKey{hash.digest()};
}

std::optional<CryptoStreamKey> from_cached_header(http::response_header<> const& hdr) {
    auto i = hdr.find(http_::response_uri_hdr);
    if (i == hdr.end()) {
        return {};
    }
    return from_url(i->value());
}

} // namespace
