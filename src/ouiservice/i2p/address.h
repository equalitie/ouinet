#pragma once

#include <string>
#include <optional>
#include <ostream>
#include <span>
#include <cstdint>
#include "api.h"

namespace ouinet {

class OUINET_I2P_API I2pAddress {
public:
    static constexpr size_t B32_ADDR_BINARY_SIZE = 32; // Size of sha256 hash

    std::string value;

    OUINET_I2P_API
    static std::optional<I2pAddress> parse(std::string_view);

    static std::optional<I2pAddress> from_binary_b32(std::span<unsigned char>);

    I2pAddress(I2pAddress const&) = default;
    I2pAddress(I2pAddress &&) = default;
    I2pAddress& operator=(I2pAddress const&) = default;
    I2pAddress& operator=(I2pAddress &&) = default;

    auto operator<=>(const I2pAddress&) const = default;

    friend std::ostream& operator<<(std::ostream& os, I2pAddress const& addr) {
        return os << addr.value;
    }

    static std::optional<std::string> b64_to_b32(std::string);

private:
    I2pAddress(std::string value) : value(std::move(value)) {}
};

} // ouinet namespace
