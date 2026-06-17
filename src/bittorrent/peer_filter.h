#pragma once

#include <boost/asio/ip/udp.hpp>
#include "api.h"

namespace ouinet::bittorrent {

class OUINET_COMMON_API PeerFilter {
    enum class Kind {
        none,
        martian
    };

    Kind _kind;

    explicit PeerFilter(Kind kind) : _kind(kind) {}

public:

    // Allows any endpoint. Useful mainly for tests.
    const static PeerFilter none;

    // Filters out "martians", that is, peers with invalid/suspicious endpoints. This is the default.
    const static PeerFilter martian;

    // Returns whether the peer is allowed according to this filter.
    bool is_allowed(const boost::asio::ip::udp::endpoint& ep) const;
};

} // namespace ouinet::bittorrent
