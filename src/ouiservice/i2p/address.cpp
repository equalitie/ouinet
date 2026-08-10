#include "address.h"
#include <string_view>
#include "util/hash.h"
#include "util/overloaded.h"
#include <boost/beast/core/detail/base64.hpp>

namespace ouinet {

// Validate I2P address:
// https://i2p.net/en/docs/overview/naming/

using B32 = I2pAddress::B32;
using B64 = I2pAddress::B64;

bool is_valid_b32(std::string_view s) {
    if (!s.ends_with(B32::SUFFIX)) return false;

    // Traditional b32 addresses are exactly 52 encoded characters without .b32.i2p
    // b33 extended addresses are 56+ encoded characters
    // these are used for encrypted lease-sets
    size_t label_len = s.size() - B32::SUFFIX.size();
    if (label_len!= 52 && label_len < 56) return false;

    for (size_t i = 0; i < label_len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

bool is_valid_b64(std::string_view s) {
    // It MAY have the suffix
    if (s.ends_with(B64::SUFFIX)) {
        s.remove_suffix(B64::SUFFIX.size());
    }

    // B64 addresses contain keys which are at least 516 bytes long
    // in base64 presentation
    if (s.size() < 516) return false;

    // Remove base64 padding
    while (s.ends_with("=")) {
        s.remove_suffix(1);
    }


    // Base64 used throughout I2P is not standard base64
    // "/" becomes "~" to avoid filesystem conflicts
    // "+" becomes "-" for URL-safe encoding without percent-encoding requirements
    // https://pkg.go.dev/github.com/go-i2p/common/base64
    static const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-~";

    for (size_t i = 0; i < s.size(); ++i) {
        if (alphabet.find(s[i]) == std::string_view::npos) return false;
    }

    return true;
}

/* static */
std::optional<B32> B32::parse(std::string_view s) {
    if (is_valid_b32(s)) return B32(std::string(s));
    return {};
}

/* static */
std::optional<B64> B64::parse(std::string_view s) {
    if (is_valid_b64(s)) return B64(std::string(s));
    return {};
}

/* static */
std::optional<I2pAddress> I2pAddress::parse(std::string_view s) {
    if (auto a = B32::parse(s)) return a;
    if (auto a = B64::parse(s)) return a;
    return {};
}

static std::string remove_padding(std::string s) {
    while (s.ends_with('=')) {
        s.resize(s.size() - 1);
    }
    return s;
}

static std::string b64_i2p_to_standard(std::string s) {
    for (auto& c : s) {
        if (c == '-') c = '+';
        else if (c == '~') c = '/';
    }
    return s;
}

static std::optional<std::vector<char>> decode_b64(std::string in) {
    using namespace boost::beast::detail::base64;
    in = remove_padding(b64_i2p_to_standard(std::move(in)));
    std::vector<char> out;
    out.resize(decoded_size(in.size()));
    auto [size, used] = decode(out.data(), in.data(), in.size());
    if (used != in.size()) {
        return {};
    }
    out.resize(size);
    return out;
}

std::string encode_b32(std::span<unsigned char> in, bool with_padding = false) {
    constexpr std::string_view base32_alphabet = "abcdefghijklmnopqrstuvwxyz234567";

    if (in.empty()) {
        return {};
    }

    std::string output;
    output.reserve(((in.size() + 4) / 5) * 8);

    std::uint64_t buffer = 0;
    int bits_left = 0;

    for (const auto b : in) {
        buffer = (buffer << 8) | b;
        bits_left += 8;

        while (bits_left >= 5) {
            bits_left -= 5;

            const auto index =
                static_cast<std::size_t>((buffer >> bits_left) & 0x1F);

            output.push_back(base32_alphabet[index]);
        }
    }

    // Remaining bits
    if (bits_left > 0) {
        const auto index =
            static_cast<std::size_t>((buffer << (5 - bits_left)) & 0x1F);

        output.push_back(base32_alphabet[index]);
    }

    // RFC 4648 padding
    if (with_padding) {
        while (output.size() % 8 != 0) {
            output.push_back('=');
        }
    }

    return output;
}

static
std::optional<std::string> b64_to_b32(std::string in) {
    auto binary = decode_b64(std::move(in));
    if (!binary) return {};
    auto digest = util::sha256_digest(std::string_view(binary->begin(), binary->end()));
    return encode_b32(std::span(digest.begin(), digest.end()));
}

/* static */
std::optional<B32> B32::from_binary(std::span<unsigned char> bytes) {
    if (bytes.size() != B32::BYTE_SIZE) return {};
    return B32(encode_b32(bytes) += B32::SUFFIX);
}

const std::string& I2pAddress::as_str() const {
    return visit([] (auto& addr) -> auto& { return addr.value; });
}

B32 B64::to_b32() const {
    auto b32 = b64_to_b32(value);
    if (!b32) throw std::runtime_error("Invalid I2P b64 address in `to_b32` conversion");
    return B32{*b32 += B32::SUFFIX};
}

B32 I2pAddress::to_b32() const {
    return visit(overloaded{
            [] (const B32& addr) -> B32 { return addr; },
            [] (const B64& addr) -> B32 { return addr.to_b32(); }
        });
}

std::ostream& operator<<(std::ostream& os, I2pAddress const& addr) {
    return addr.visit([&os] (auto& addr) -> auto& { return os << addr; });
}

std::ostream& operator<<(std::ostream& os, B32 const& addr) {
    return os << addr.value;
}

std::ostream& operator<<(std::ostream& os, B64 const& addr) {
    return os << addr.value;
}

} // namespace ouinet
