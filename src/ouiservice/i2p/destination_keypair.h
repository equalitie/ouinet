#pragma once

#include "api.h"
#include <string>
#include <string_view>
#include <optional>

namespace ouinet {

struct OUINET_I2P_API I2pDestinationKeypair {
    std::string pub;
    std::string priv;

    auto operator<=>(const I2pDestinationKeypair&) const = default;

    std::string to_json_string() const;
    static std::optional<I2pDestinationKeypair> from_json_string(std::string_view);
};

} // namespace
