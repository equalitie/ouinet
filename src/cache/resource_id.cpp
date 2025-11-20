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

template<class CharT>
inline bool is_hex(CharT c) {
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f');
}

template<class CharT>
inline std::optional<std::string> sanitize_hex(std::basic_string_view<CharT> hex) {
    auto hex_size = util::SHA1::size() * 2;

    if (hex.size() != hex_size) {
        return {};
    }

    std::string ret(hex_size, '0');

    for (size_t i = 0; i < hex_size; ++i) {
        if (is_hex(hex[i])) {
            ret[i] = char(hex[i]);
        } else {
            return {};
        }
    }

    return ret;
}

std::optional<ResourceId> ResourceId::from_hex(std::string_view hex) {
    auto sanitized = sanitize_hex(hex);
    if (!sanitized) return {};
    return ResourceId(std::move(*sanitized));
}

std::optional<ResourceId> ResourceId::from_hex(std::wstring_view hex) {
    auto sanitized = sanitize_hex(hex);
    if (!sanitized) return {};
    return ResourceId(std::move(*sanitized));
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
