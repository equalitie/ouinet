#pragma once

#include <string>
#include <optional>
#include <ostream>
#include <span>
#include <cstdint>
#include <variant>
#include "api.h"

namespace ouinet {

class OUINET_I2P_API I2pAddress {
public:
    struct OUINET_I2P_API B32 {
        static constexpr size_t BYTE_SIZE = 32; // Size of sha256 hash
        // b32 format always has SUFFIX when printed and parsed.
        static constexpr std::string_view SUFFIX = ".b32.i2p";

        OUINET_I2P_API
        static std::optional<B32> parse(std::string_view);

        OUINET_I2P_API
        static std::optional<B32> from_binary(std::span<unsigned char>);

        auto operator<=>(const B32&) const = default;

        const std::string& as_str() const { return value; }

        // This string includes the SUFFIX
        std::string value;
    };

    struct OUINET_I2P_API B64 {
        // b64 format MAY have the SUFFIX when printed or parsed, but most of
        // the times it doesn't
        static constexpr std::string_view SUFFIX = ".b64.i2p";

        OUINET_I2P_API
        static std::optional<B64> parse(std::string_view);

        auto operator<=>(const B64&) const = default;

        B32 to_b32() const;

        const std::string& as_str() const { return value; }

        // This string does NOT include the SUFFIX
        std::string value;
    };

    using Alternatives = std::variant<B32, B64>;

    template<class V>
    requires(!std::is_same_v<V, I2pAddress> && std::constructible_from<Alternatives, V>)
    I2pAddress(V&& v) : value(std::forward<V>(v)) {}

    OUINET_I2P_API
    static std::optional<I2pAddress> parse(std::string_view);

    auto operator<=>(const I2pAddress&) const = default;

    B32 to_b32() const;

    const std::string& as_str() const;

    OUINET_I2P_API friend std::ostream& operator<<(std::ostream&, I2pAddress const&);
    OUINET_I2P_API friend std::ostream& operator<<(std::ostream&, B32 const&);
    OUINET_I2P_API friend std::ostream& operator<<(std::ostream&, B64 const&);

    template<class Visitor, class Self>
    decltype(auto) visit(this Self&& self, Visitor&& visitor) {
        return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self).value);
    }

private:

    Alternatives value;
};

} // ouinet namespace
