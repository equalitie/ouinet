#pragma once

#include <boost/asio/ip/tcp.hpp>

namespace ouinet { namespace bittorrent {

inline
std::string encode_endpoint(asio::ip::udp::endpoint endpoint)
{
    std::string output;
    if (endpoint.address().is_v4()) {
        std::array<unsigned char, 4> ip_bytes = endpoint.address().to_v4().to_bytes();
        output.append((char *)ip_bytes.data(), ip_bytes.size());
    } else {
        std::array<unsigned char, 16> ip_bytes = endpoint.address().to_v6().to_bytes();
        output.append((char *)ip_bytes.data(), ip_bytes.size());
    }
    unsigned char p1 = (endpoint.port() >> 8) & 0xff;
    unsigned char p2 = (endpoint.port() >> 0) & 0xff;
    output += p1;
    output += p2;
    return output;
}

inline
std::string encode_endpoint(asio::ip::tcp::endpoint endpoint)
{
    return encode_endpoint(asio::ip::udp::endpoint( endpoint.address()
                                                  , endpoint.port()));
}

inline
boost::optional<asio::ip::udp::endpoint>
decode_endpoint(boost::string_view endpoint)
{
    namespace ip = asio::ip;
    using ip::udp;

    if (endpoint.size() == 6) {
        std::array<unsigned char, 4> ip_bytes;
        std::copy(endpoint.begin(), endpoint.begin() + ip_bytes.size(), ip_bytes.data());
        uint16_t port = ((uint16_t)(unsigned char)endpoint[4]) << 8
                      | ((uint16_t)(unsigned char)endpoint[5]) << 0;
        return udp::endpoint(ip::address_v4(ip_bytes), port);
    } else if (endpoint.size() == 18) {
        std::array<unsigned char, 16> ip_bytes;
        std::copy(endpoint.begin(), endpoint.begin() + ip_bytes.size(), ip_bytes.data());
        uint16_t port = ((uint16_t)(unsigned char)endpoint[16]) << 8
                      | ((uint16_t)(unsigned char)endpoint[17]) << 0;
        return udp::endpoint(ip::address_v6(ip_bytes), port);
    } else {
        return boost::none;
    }
}

}} // namespaces
