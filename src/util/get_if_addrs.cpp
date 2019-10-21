#include "get_if_addrs.h"
#include <ifaddrs.h>

using namespace ouinet;
using namespace std;

set<asio::ip::address> util::get_if_addrs(sys::error_code& ec)
{
    set<asio::ip::address> output;

    struct ifaddrs* ifaddrs;

    if (getifaddrs(&ifaddrs)) {
        ec = make_error_code(static_cast<boost::system::errc::errc_t>(errno));
        return output;
    }

    for (struct ifaddrs* ifaddr = ifaddrs; ifaddr != nullptr; ifaddr = ifaddr->ifa_next) {
        if (!ifaddr->ifa_addr) {
            continue;
        }

        if (ifaddr->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)ifaddr->ifa_addr;
            asio::ip::address_v4 ip(ntohl(addr->sin_addr.s_addr));
            output.insert(ip);
        } else if (ifaddr->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* addr = (struct sockaddr_in6*)ifaddr->ifa_addr;
            std::array<unsigned char, 16> address_bytes;
            memcpy(address_bytes.data(), addr->sin6_addr.s6_addr, address_bytes.size());
            asio::ip::address_v6 ip(address_bytes, addr->sin6_scope_id);
            output.insert(ip);
        }
    }

    freeifaddrs(ifaddrs);

    return output;
}

