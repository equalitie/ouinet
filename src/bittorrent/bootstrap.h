#pragma once

#include <ostream>
#include <string>

#include <boost/asio/ip/udp.hpp>
#include <boost/optional.hpp>
#include <boost/variant.hpp>

#include "../namespaces.h"

namespace ouinet {
namespace bittorrent {
namespace bootstrap {

static const unsigned short default_port = 6881;

using Address = boost::variant< asio::ip::udp::endpoint
                              , asio::ip::address
                              , std::string /* domain_name[:port] */>;

boost::optional<Address>
parse_address(const std::string& addr);

// Represent the address as `<HOST>` or `<HOST>:<PORT>`,
// where `<HOST>` can be a host name, `<IPv4>` address, or `<[IPv6]>` address (bracketed).
std::ostream&
operator<<(std::ostream&, const Address&);

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
