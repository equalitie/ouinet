#include "resource_id.h"
#include "../util/bytes.h"
#include "../util/hash.h"
#include "../util/url.h"
#include "../util.h"

namespace ouinet::cache {

static constexpr size_t BYTE_SIZE = util::SHA1::size() /* = 20 bytes */;

template<class CharT>
inline bool is_hex(CharT c) {
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f');
}

template<class CharT>
inline std::optional<std::string> sanitize_hex(std::basic_string_view<CharT> hex) {
    auto hex_size = BYTE_SIZE * 2;

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

ResourceId::ResourceId(std::string repr)
    : _repr(std::move(repr))
{
    assert(_repr.size() == BYTE_SIZE * 2);
}

ResourceId ResourceId::from_url(std::string_view url_str) {
    util::SHA1 hash;
    hash.update("ouinet-resource-id-salt");
    hash.update(url_str);
    return ResourceId(util::bytes::to_hex(hash.close()));
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

std::ostream& operator<<(std::ostream& os, const ouinet::cache::ResourceId& id) {
    return os << id.hex_string();
}

} // namespace ouinet::cache

