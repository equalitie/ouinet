#include "resource_key.h"
#include "util/hash.h"
#include "constants.h"

namespace ouinet::cache::resource_key {

static const std::string_view SALT = "ouinet-resource-key-salt";

CryptoStreamKey from(std::string_view url) {
    util::SHA256 hash;
    hash.update(SALT);
    hash.update(url);
    return CryptoStreamKey{hash.digest()};
}

std::optional<CryptoStreamKey> from(http::response_header<> const& hdr) {
    auto i = hdr.find(http_::response_uri_hdr);
    if (i == hdr.end()) {
        return {};
    }
    return from(i->value());
}

} // namespace
