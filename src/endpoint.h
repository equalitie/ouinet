#pragma once

#include <boost/variant.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <optional>
#include <string>
#include "ouiservice/i2p/address.h"
#include "namespaces.h"

namespace ouinet {

class Endpoint {
public:
    struct Utp {
        asio::ip::udp::endpoint value;

        bool operator<(const Utp& other) const { return value < other.value; }
        bool operator==(const Utp& other) const { return value == other.value; }
    };

    struct Bep5 {
        std::string value;

        auto operator<=>(const Bep5&) const = default;
    };

    using Alternatives = std::variant<
        asio::ip::tcp::endpoint,
        Utp,
        Bep5,
        I2pAddress
    >;

    bool operator<(const Endpoint& other) const { return _alternative < other._alternative; }
    bool operator==(const Endpoint& other) const { return _alternative == other._alternative; }

    static std::optional<Endpoint> parse(std::string_view);

    friend
    std::ostream& operator<<(std::ostream&, const Endpoint&);

    Endpoint() = default;
    Endpoint(Endpoint const&) = default;
    Endpoint(Endpoint &&) = default;

    Endpoint& operator=(Endpoint const&) = default;
    Endpoint& operator=(Endpoint &&) = default;

    template<class T>
    requires(!std::same_as<std::remove_cvref_t<T>, Endpoint>)
    Endpoint(T&& ep)
        : _alternative(std::forward<T>(ep)) {}

    template<class T>
    T* get_if() {
        return std::get_if<T>(&_alternative);
    }

private:
    Alternatives _alternative;
};

} // namespace
