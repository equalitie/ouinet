#pragma once

#include <ostream>
#include <set>
#include <string>

#include <boost/asio/ip/udp.hpp>
#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/variant.hpp>
#include <vector>

#include "namespaces.h"
#include "api.h"

namespace ouinet {
namespace bittorrent {
namespace bootstrap {

static const unsigned short default_port = 6881;

using Address = boost::variant< asio::ip::udp::endpoint
                              , asio::ip::address
                              , std::string /* domain_name[:port] */>;

// Parse an address in `<HOST>` or `<HOST>:<PORT>` format,
// where `<HOST>` can be a host name, `<IPv4>` address, or `<[IPv6]>` address (bracketed).
// Host names are always converted to lower case.
OUINET_COMMON_API
boost::optional<Address>
parse_address(const std::string& addr);

OUINET_COMMON_API
boost::optional<Address>
parse_address(boost::string_view addr);

// Represent the address as `<HOST>` or `<HOST>:<PORT>`,
// where `<HOST>` can be a host name, `<IPv4>` address, or `<[IPv6]>` address (bracketed).
OUINET_COMMON_API
std::ostream&
operator<<(std::ostream&, const Address&);

// Default bootstrap servers
const std::vector<Address> default_servers {
    "dht.libtorrent.org:25401",
    "dht.transmissionbt.com:6881",

    // Alternative bootstrap servers from the Ouinet project.
    "router.bt.ouinet.work",

    // Part of previous name (in case of DNS failure).
    asio::ip::make_address("74.3.163.127"),

    // squat popular UDP high port (SIP)
    "routerx.bt.ouinet.work:5060"
};

// Bootstrap servers configuration
struct Config {
    // Use the default bootstrap servers. Enabled by default.
    bool _default = true;
    // Extra bootstrap servers to use, in addition to the default ones. Empty by default.
    std::set<Address> _extra;

    Config& with_default(bool enabled) {
        _default = enabled;
        return *this;
    }

    Config& with_extras(std::set<Address> addrs) {
        _extra = std::move(addrs);
        return *this;
    }

    // Returns all configured bootstrap servers
    std::vector<Address> collect() const;
};

} // bootstrap namespace
} // bittorrent namespace
} // ouinet namespace

namespace boost {

// This is needed since `Address` is just an alias.
inline
std::ostream&
operator<<(std::ostream& o, const ouinet::bittorrent::bootstrap::Address& a) {
    return ouinet::bittorrent::bootstrap::operator<<(o, a);
}

} // ouinet namespace
