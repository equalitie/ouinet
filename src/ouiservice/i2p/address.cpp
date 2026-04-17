// Validate I2P address:
// https://i2p.net/en/docs/overview/naming/

#include <string>

namespace ouinet::ouiservice::i2poui {

bool isValidI2PB32(const std::string &s) {
    const std::string suffix = ".b32.i2p";
    if (s.find(suffix) == std::string::npos) return false;

    size_t label_len = s.size() - suffix.size();
    if (label_len!= 52 && label_len!=56) return false;

    for (size_t i = 0; i < label_len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    }
    return true;
}

} //namespace ouinet::ouiservice::i2poui
