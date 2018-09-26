#pragma once

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
boost::optional<asio::ip::udp::endpoint> decode_endpoint(std::string endpoint)
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

inline
bool decode_contacts_v4( const std::string& str
                       , std::vector<dht::NodeContact>& contacts)
{
    // 20 bytes of ID, plus 6 bytes of endpoint
    if (str.size() % 26) { return false; }

    for (unsigned int i = 0; i < str.size() / 26; i++) {
        std::string encoded_contact = str.substr(i * 26, 26);

        contacts.push_back({
            NodeID::from_bytestring(encoded_contact.substr(0, 20)),
            *decode_endpoint(encoded_contact.substr(20))
        });
    }

    return true;
}

inline
bool decode_contacts_v6( const std::string& str
                       , std::vector<dht::NodeContact>& contacts)
{
    // 20 bytes of ID, plus 18 bytes of endpoint
    if (str.size() % 38) { return false; }

    for (unsigned int i = 0; i < str.size() / 38; i++) {
        std::string encoded_contact = str.substr(i * 38, 38);

        contacts.push_back({
            NodeID::from_bytestring(encoded_contact.substr(0, 20)),
            *decode_endpoint(encoded_contact.substr(20))
        });
    }

    return true;
}

}} // namespaces
