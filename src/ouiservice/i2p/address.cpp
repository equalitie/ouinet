#include "address.h"
#include <string_view>

namespace ouinet {

// Validate I2P address:
// https://i2p.net/en/docs/overview/naming/
/* static */
bool I2pAddress::is_valid_b32(std::string_view s) {
    static const std::string_view suffix = ".b32.i2p";

    if (!s.ends_with(suffix)) return false;

    size_t label_len = s.size() - suffix.size();
    if (label_len!= 52 && label_len < 56) return false;

    for (size_t i = 0; i < label_len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

/* static */
bool I2pAddress::is_valid_b64(std::string_view s) {
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

} // namespace ouinet

