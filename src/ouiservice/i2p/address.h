#pragma once

#include <string>
#include <optional>
#include <ostream>

namespace ouinet {

class I2pAddress {
public:
    std::string value;

    static std::optional<I2pAddress> parse(std::string_view);

    I2pAddress(I2pAddress const&) = default;
    I2pAddress(I2pAddress &&) = default;
    I2pAddress& operator=(I2pAddress const&) = default;
    I2pAddress& operator=(I2pAddress &&) = default;

    auto operator<=>(const I2pAddress&) const = default;

    friend std::ostream& operator<<(std::ostream& os, I2pAddress const& addr) {
        return os << addr.value;
    }

private:
    I2pAddress(std::string value) : value(std::move(value)) {}
};

} // ouinet namespace
