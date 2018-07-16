#include "dht.h"

#include <iostream>

#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <boost/asio/ip/address.hpp>

namespace asio = boost::asio;
using udp = asio::ip::udp;

using std::cerr;
using std::endl;
using namespace ouinet::bittorrent;

std::vector<asio::ip::address> linux_get_addresses()
{
    std::vector<asio::ip::address> output;

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
            asio::ip::address_v4 ip(ntohl(addr->sin_addr.s_addr));
            output.push_back(ip);
        } else if (ifaddr->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* addr = (struct sockaddr_in6*)ifaddr->ifa_addr;
            std::array<unsigned char, 16> address_bytes;
            memcpy(address_bytes.data(), addr->sin6_addr.s6_addr, address_bytes.size());
            asio::ip::address_v6 ip(address_bytes, addr->sin6_scope_id);
            output.push_back(ip);
        }
    }

    freeifaddrs(ifaddrs);

    // TODO: filter unroutable addresses
    return output;
}

std::vector<asio::ip::address>
filter( bool loopback
      , bool ipv4
      , bool ipv6
      , const std::vector<asio::ip::address>& ifaddrs)
{
    std::vector<asio::ip::address> ret;

    for (auto addr : ifaddrs) {
        if (addr.is_loopback() && !loopback) continue;
        if (addr.is_v4()       && !ipv4)     continue;
        if (addr.is_v6()       && !ipv6)     continue;
        ret.push_back(addr);
    }

    return ret;
}

void usage(std::ostream& os, const char* app_name) {
    os << "Usage:" << endl
       << "  " << app_name << " [interface-address]" << endl
       << "E.g.:" << endl
       << "  " << app_name << "              # All non loopback interfaces" << endl
       << "  " << app_name << " 0.0.0.0      # Any ipv4 interface" << endl
       << "  " << app_name << " 192.168.0.1  # Concrete interface" << endl;
}

int main(int argc, const char** argv)
{
    asio::io_service ios;

    MainlineDht dht(ios);

    std::vector<asio::ip::address> ifaddrs;

    if (argc <= 1) {
        ifaddrs = filter( false
                        , true
                        , true
                        , linux_get_addresses());
    }
    else if (std::string(argv[1]) == "-h") {
        usage(std::cout, argv[0]);
        return 0;
    }
    else {
        boost::system::error_code ec;
        ifaddrs.push_back(asio::ip::make_address(argv[1], ec));

        if (ec) {
            std::cerr << "Failed parsing \"" << argv[1] << "\" as an IP "
                      << "address: " << ec.message() << std::endl;
            usage(std::cerr, argv[0]);
            return 1;
        }
    }

    for (auto addr : ifaddrs) {
        std::cout << "Spawning DHT node on " << addr << std::endl;
    }


    asio::spawn(ios, [&] (auto yield) {
        dht.set_interfaces(ifaddrs, yield);
        //dht.tracker_get_peers();
    });

    ios.run();
}
