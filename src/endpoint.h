#pragma once

#include <boost/variant.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>
#include "split_string.h"

namespace ouinet {

struct Endpoint {
    enum Type {
        TcpEndpoint,
        I2pEndpoint,
#ifdef USE_GNUNET
        GnunetEndpoint,
#endif
        LampshadeEndpoint,
        Obfs2Endpoint,
        Obfs3Endpoint,
        Obfs4Endpoint
    };

    Type type;
    std::string endpoint_string;

    bool operator==(const Endpoint& other) const {
        return type == other.type && endpoint_string == other.endpoint_string;
    }
};

boost::optional<Endpoint> parse_endpoint(beast::string_view endpoint);

std::ostream& operator<<(std::ostream& os, const Endpoint&);

} // ouinet namespace
