#pragma once

#include <memory>
#include <string_view>
#include "api.h"

namespace ouinet {

namespace sign {
    class PublicKey;
}

namespace bittorrent {
    struct NodeID;
}

class OUINET_COMMON_API SwarmCalculator {
public:
    SwarmCalculator(sign::PublicKey cache_http_public_key);

    std::string swarm_name(std::string_view group) const;

    bittorrent::NodeID swarm_id(std::string_view group) const;

private:
    // Pimpl
    std::shared_ptr<sign::PublicKey> _cache_http_pub_key;
};

} // namespace
