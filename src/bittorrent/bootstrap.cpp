#include "bootstrap.h"

#include <tuple>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>
#include <boost/utility/string_view.hpp>

#include "../parse/number.h"
#include "../util.h"
#include "../util/str.h"
#include "../util/variant.h"

namespace ouinet {
namespace bittorrent {
namespace bootstrap {

boost::optional<Address>
parse_address(const std::string& addr) {
    boost::string_view hp(addr), host_v, port_v;
    std::tie(host_v, port_v) = util::split_ep(hp);

    if (host_v.empty()) return boost::none;

    // Try to get a port number.
    unsigned short port_n = 0;  // no port
    if (!port_v.empty()) {
        auto port_o = parse::number<unsigned short>(port_v);
        if (!port_o) return boost::none;
        port_n = *port_o;
    }

    // Try to interpret host as IP address.
    auto host = host_v.to_string();
    {
        sys::error_code ec;
        auto ip_addr = asio::ip::make_address(host, ec);
        if (!ec) {
            if (!port_n) return Address{ip_addr};
            else return Address{asio::ip::udp::endpoint(ip_addr, port_n)};
        }
    }

    // Parse as host name.
    boost::algorithm::to_lower(host);

    static const boost::regex lhost_rx("^[_0-9a-z]+(?:\\.[_0-9a-z]+)*$");
    if (!boost::regex_match(host, lhost_rx)) return boost::none;

    if (!port_n) return Address{host};
    return Address{util::str(host, ':', port_n)};
}

static void
print_ip_address(std::ostream& os, const asio::ip::address& ad) {
    if (ad.is_v6()) os << '[';
    os << ad;
    if (ad.is_v6()) os << ']';
}

std::ostream&
operator<<(std::ostream& os, const Address& a) {
    util::apply
        ( a
        , [&os] (const asio::ip::udp::endpoint& ep) {
              print_ip_address(os, ep.address());
              os << ':' << ep.port();
        }
        , [&os] (const asio::ip::address& ad) {
              print_ip_address(os, ad);
          }
        , [&os] (const std::string& s) { os << s; }
        );
    return os;
}

} // bootstrap namespace
} // bittorrent namespace
} // ouinet namespace
