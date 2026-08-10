#include "endpoint.h"
#include "util/overloaded.h"
#include "parse/number.h"

namespace ouinet {

// TODO: Use parse/endpoint.h (it uses boost::{optional,string_view} though).
template<class TcpOrUdp>
static std::optional<typename TcpOrUdp::endpoint> parse_ip_endpoint(std::string_view s) {
    auto pos = s.find(':');
    if (pos == std::string::npos) return {};

    sys::error_code ec;
    auto addr = asio::ip::make_address(s.substr(0, pos), ec);
    if (ec) return {};

    auto port_s = s.substr(pos + 1);
    auto port = parse::number<unsigned short>(port_s);
    if (!port) return {};

    return typename TcpOrUdp::endpoint(addr, *port);
}

/* static */
std::optional<Endpoint> Endpoint::parse(std::string_view s) {
    size_t pos = s.find(':');
    if (pos == std::string::npos) return {};

    std::string_view type = s.substr(0, pos);
    auto ep_s = s.substr(pos + 1);

    if (type == "tcp") {
        auto ep = parse_ip_endpoint<asio::ip::tcp>(ep_s);
        if (!ep) return {};
        return *ep;
    } else if (type == "utp") {
        auto ep = parse_ip_endpoint<asio::ip::udp>(ep_s);
        if (!ep) return {};
        return Utp { *ep };
    } else if (type == "i2p") {
        auto addr = I2pAddress::parse(ep_s);
        if (!addr) return {};
        return *addr;
    } else if (type == "bep5") {
        return Bep5 { std::string(ep_s) };
    } else {
        return {};
    }
}

std::ostream& operator<<(std::ostream& os, const Endpoint& ep) {
    std::visit(overloaded {
            [&] (asio::ip::tcp::endpoint const& ep) mutable {
                os << "tcp:" << ep;
            },
            [&] (Endpoint::Utp const& ep) mutable {
                os << "utp:" << ep.value;
            },
            [&] (I2pAddress const& ep) mutable {
                os << "i2p:" << ep;
            },
            [&] (Endpoint::Bep5 const& ep) mutable {
                os << "bep5:" << ep.value;
            },
        },
        ep._alternative);

    return os;
}

} // ouinet
