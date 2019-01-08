#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>

#include <string>

namespace ouinet {
namespace ouiservice {
namespace pt {

/*
 * Escape a string by prefixing all $characters with backslashes, as well
 * as the backslash character.
 */
inline std::string string_escape(std::string payload, std::string characters)
{
    std::string output;
    for (auto c : payload) {
        if (c == '\\' || characters.find(c) != std::string::npos) {
            output += '\\';
        }
        output += c;
    }
    return output;
}

/*
 * Parse a PT-encoded endpoint:
 * - 1.2.3.4:567
 * - [1:2:3:4::5]:678
 */
inline boost::optional<asio::ip::tcp::endpoint> parse_endpoint(std::string endpoint)
{
    size_t pos = endpoint.rfind(':');
    if (pos == std::string::npos) {
        return boost::none;
    }
    int port;
    try {
        port = std::stoi(endpoint.substr(pos + 1));
    } catch(...) {
        return boost::none;
    }
    if (port < 0 || port > 65535) {
        return boost::none;
    }

    std::string address_string = endpoint.substr(0, pos);
    if (
           address_string.size() > 0
        && address_string[0] == '['
        && address_string[address_string.size() - 1] == ']'
    ) {
        address_string = address_string.substr(1, address_string.size() - 2);
    }
    sys::error_code ec;
    asio::ip::address address = asio::ip::address::from_string(address_string, ec);
    if (ec) {
        return boost::none;
    }

    return asio::ip::tcp::endpoint(address, (short)port);
}

inline std::string format_endpoint(asio::ip::tcp::endpoint endpoint)
{
    if (endpoint.address().is_v4()) {
        return endpoint.address().to_v4().to_string() + ":" + std::to_string(endpoint.port());
    } else {
        return "[" + endpoint.address().to_v6().to_string() + "]:" + std::to_string(endpoint.port());
    }
}

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
