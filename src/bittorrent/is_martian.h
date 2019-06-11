#pragma once

#include <boost/asio/ip/udp.hpp>
#include "../namespaces.h"

namespace ouinet { namespace bittorrent {

inline bool is_martian(const asio::ip::udp::endpoint& ep) {
    if (ep.port() == 0) return true;
    auto addr = ep.address();

    if (addr.is_v4()) {
        auto v4 = addr.to_v4();

        if (v4.is_multicast()) return true;
        if (v4.is_loopback())  return true;

        if (v4.to_bytes()[0] == 0) return true;
    }
    else {
        auto v6 = addr.to_v6();

        if (v6.is_multicast())   return true;
        if (v6.is_link_local())  return true;
        if (v6.is_v4_mapped())   return true;
        if (v6.is_loopback())    return true;
        if (v6.is_unspecified()) return true;
    }

    return false;
}

}} // namespace
