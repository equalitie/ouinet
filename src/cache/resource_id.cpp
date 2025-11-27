#include "resource_id.h"
#include "../util/bytes.h"
#include "../util/hash.h"
#include "../util/url.h"
#include "../util.h"

namespace ouinet::cache {

// TODO: This is arbitrary, previous implementations used SHA1(url) as resource ID
// and here it's kept for compatibility, probably nothing will break if we used
// a different size. SHA1 lenght is 20 bytes and an example Scrypt from OpenSSL uses
// 64 bytes. 
static constexpr size_t BYTE_SIZE = util::SHA1::size();

// Took these values from https://docs.openssl.org/1.1.1/man7/scrypt/
// and increased the N.
// TODO: Check if they are reasonable
static constexpr uint64_t SCRYPT_N = 1 << 14;
static constexpr uint64_t SCRYPT_r = 8;
static constexpr uint64_t SCRYPT_p = 1;

std::optional<ResourceId> ResourceId::from_url(std::string_view url_str, YieldContext yield) {
    auto url = util::Url::from(util::to_boost(url_str));
    if (!url) return {};
    std::string cache_url = util::canonical_url(std::move(*url));
    if (cache_url.empty()) return {};
    util::ScryptParams params{
        SCRYPT_N,
        SCRYPT_r,
        SCRYPT_p
    };
    auto key = util::ScryptWorker::global_worker.derive<BYTE_SIZE>(cache_url, "ouinet-resource-id", params, yield);
    auto hex_digest = util::bytes::to_hex(key);
    return ResourceId(std::move(hex_digest));
}

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
