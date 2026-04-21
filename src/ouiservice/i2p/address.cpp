#include "address.h"

namespace ouinet {

// Validate I2P address:
// https://i2p.net/en/docs/overview/naming/
/* static */
bool I2pAddress::is_valid_b32(std::string_view s) {
    static const std::string_view suffix = ".b32.i2p";

    if (!s.ends_with(suffix)) return false;

    size_t label_len = s.size() - suffix.size();
    if (label_len!= 52 && label_len!=56) return false;

    for (size_t i = 0; i < label_len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

bool is_valid_b64(std::string_view s) {
    // TODO
    return !s.empty();
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

