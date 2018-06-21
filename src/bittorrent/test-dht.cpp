#include "dht.h"

#include <iostream>

#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

std::vector<boost::asio::ip::address> linux_get_addresses()
{
    std::vector<boost::asio::ip::address> output;

    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs)) {
        exit(1);
    }

    for (struct ifaddrs* ifaddr = ifaddrs; ifaddr != nullptr; ifaddr = ifaddr->ifa_next) {
        if (!ifaddr->ifa_addr) {
            continue;
        }

        if (ifaddr->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)ifaddr->ifa_addr;
            boost::asio::ip::address_v4 ip(ntohl(addr->sin_addr.s_addr));
            output.push_back(ip);
            std::cout << ip.to_string() << "\n";
        } else if (ifaddr->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* addr = (struct sockaddr_in6*)ifaddr->ifa_addr;
            std::array<unsigned char, 16> address_bytes;
            memcpy(address_bytes.data(), addr->sin6_addr.s6_addr, address_bytes.size());
            boost::asio::ip::address_v6 ip(address_bytes, addr->sin6_scope_id);
            output.push_back(ip);
            std::cout << ip.to_string() << "\n";
        }
    }

    freeifaddrs(ifaddrs);

    // TODO: filter unroutable addresses
    return output;
}

int main()
{
    boost::asio::io_service ios;

    ouinet::bittorrent::MainlineDht dht(ios);
    dht.set_interfaces(linux_get_addresses());

    ios.run();
}
