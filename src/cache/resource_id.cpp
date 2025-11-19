#include "resource_id.h"
#include "../util/bytes.h"
#include "../util/hash.h"
#include "../util/url.h"
#include "../util.h"

namespace ouinet::cache {

std::optional<ResourceId> ResourceId::from_url(std::string_view url_str) {
    auto url = util::Url::from(util::to_boost(url_str));
    if (!url) return {};
    auto key = util::canonical_url(std::move(*url));
    if (key.empty()) return {};
    auto key_digest = util::sha1_digest(key);
    auto hex_digest = util::bytes::to_hex(key_digest);
    return ResourceId(std::move(hex_digest));
}

inline bool is_hex(char c) {
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f');
}

std::optional<ResourceId> ResourceId::from_hex(std::string_view hex) {
    if (hex.size() != util::SHA1::size() * 2) {
        return {};
    }
    for (auto c : hex) {
        if (!is_hex(c)) {
            return {};
        }
    }
    return ResourceId(std::string(hex));
}

const std::string& ResourceId::hex_string() const {
    return _repr;
}

} // namespace ouinet::cache

namespace std {
    std::ostream& operator<<(std::ostream& os, const ouinet::cache::ResourceId& id) {
        return os << id.hex_string();
    }
} // namespace std
