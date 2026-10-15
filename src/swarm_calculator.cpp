#include "swarm_calculator.h"

#include "util/sign.h"
#include "util/hash.h"
#include "bittorrent/node_id.h"
#include "bep5_swarms.h"
#include "constants.h"

namespace ouinet {

SwarmCalculator::SwarmCalculator(sign::PublicKey cache_http_public_key) :
    _cache_http_pub_key(std::make_shared<sign::PublicKey>(std::move(cache_http_public_key)))
{}

std::string SwarmCalculator::swarm_name(std::string_view group) const {
    std::string uri_swarm_prefix = bep5::compute_uri_swarm_prefix(*_cache_http_pub_key, http_::protocol_version_current);
    return bep5::compute_uri_swarm_name(uri_swarm_prefix, {group.begin(), group.size()});
}

bittorrent::NodeID SwarmCalculator::swarm_id(std::string_view group) const {
    std::string swarm_name = this->swarm_name(group);
    util::SHA1::digest_type hash = util::sha1_digest(swarm_name);
    return bittorrent::NodeID(hash);
}

} // namespace
