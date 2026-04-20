#pragma once

// Validate I2P address:
// https://i2p.net/en/docs/overview/naming/

#include <string>

namespace ouinet {

class I2pAddress {
public:
    std::string value;

    bool operator<(const I2pAddress& other) const {
        return value < other.value;
    }

    friend std::ostream& operator<<(std::ostream& os, I2pAddress const& addr) {
        return os << addr.value;
    }
};

} // ouinet namespace

namespace ouinet::ouiservice::i2poui {

bool isValidI2PB32(const std::string &s);

} // namespace ouinet::ouiservice::i2poui
