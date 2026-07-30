#include "address.h"
#include <string_view>
#include "util/hash.h"
#include <boost/beast/core/detail/base64.hpp>

namespace ouinet {

// Validate I2P address:
// https://i2p.net/en/docs/overview/naming/

bool is_valid_b32(std::string_view s) {
    static const std::string_view suffix = ".b32.i2p";

    if (!s.ends_with(suffix)) return false;

    // Traditional b32 addresses are exactly 52 encoded characters without .b32.i2p
    // b33 extended addresses are 56+ encoded characters
    // these are used for encrypted lease-sets
    size_t label_len = s.size() - suffix.size();
    if (label_len!= 52 && label_len < 56) return false;

    for (size_t i = 0; i < label_len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

bool is_valid_b64(std::string_view s) {
    // It MAY have a suffix
    static const std::string_view suffix = ".b64.i2p";

    if (s.ends_with(suffix)) {
        s.remove_suffix(suffix.size());
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
std::optional<I2pAddress> I2pAddress::parse(std::string_view s) {
    if (is_valid_b32(s)) {
        return I2pAddress(std::string(s));
    }

    if (is_valid_b64(s)) {
        return I2pAddress(std::string(s));
    }

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

/* static */
std::optional<std::string> I2pAddress::b64_to_b32(std::string in) {
    auto binary = decode_b64(std::move(in));
    if (!binary) return {};
    auto digest = util::sha256_digest(std::string_view(binary->begin(), binary->end()));
    return encode_b32(std::span(digest.begin(), digest.end()));
}

/* static */
std::optional<I2pAddress> I2pAddress::from_binary_b32(std::span<unsigned char> bytes) {
    if (bytes.size() != B32_ADDR_BINARY_SIZE) return {};
    return I2pAddress(encode_b32(bytes) + ".b32.i2p");
}

} // namespace ouinet

