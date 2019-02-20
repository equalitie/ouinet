#pragma once

#include <boost/variant.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>
#include "split_string.h"

namespace ouinet {

struct I2PEndpoint {
    std::string pubkey;

    bool operator==(const I2PEndpoint& other) const {
        return pubkey == other.pubkey;
    }
};

using Endpoint = boost::variant< asio::ip::tcp::endpoint
                               , I2PEndpoint>;

inline
boost::optional<Endpoint> parse_endpoint(beast::string_view endpoint)
{
    using std::string;
    using beast::string_view;
    using asio::ip::tcp;

    auto as_tcp_endpoint = []( string_view host
                             , string_view port
                             ) -> boost::optional<tcp::endpoint> {
        sys::error_code ec;
        auto ip = asio::ip::address::from_string(host.to_string(), ec);
        if (ec) return boost::none;
        return tcp::endpoint(ip, strtol(port.data(), 0, 10));
    };

    sys::error_code ec;

    string_view host;
    string_view port;

    std::tie(host, port) = split_string_pair(endpoint, ':');

    if (port.empty()) {
        return Endpoint{I2PEndpoint{endpoint.to_string()}};
    }

    if (auto ep = as_tcp_endpoint(host, port)) {
        return Endpoint{*ep};
    }

    return boost::none;
}

inline
bool is_i2p_endpoint(const Endpoint& ep) {
    return boost::get<I2PEndpoint>(&ep) ? true : false;
}

std::ostream& operator<<(std::ostream& os, const I2PEndpoint&);
std::ostream& operator<<(std::ostream& os, const Endpoint&);

} // ouinet namespace
