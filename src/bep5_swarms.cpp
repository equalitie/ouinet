#include "bep5_swarms.h"
#include "util.h"

namespace ouinet { namespace bep5 {

std::string compute_injector_swarm_name(const ouinet::util::Ed25519PublicKey& pubkey, unsigned protocol_version)
{
    return util::str
        ( "ed25519:", util::base32up_encode(pubkey.serialize())
        , "/v", protocol_version
        , "/injectors");
}

std::string compute_bridge_swarm_name(const ouinet::util::Ed25519PublicKey& pubkey, unsigned protocol_version)
{
    return util::str
        ( "ed25519:", util::base32up_encode(pubkey.serialize())
        , "/v", protocol_version
        , "/bridges");
}

std::string compute_uri_swarm_prefix(const ouinet::util::Ed25519PublicKey& pubkey, unsigned protocol_version)
{
    return util::str
        ( "ed25519:", util::base32up_encode(pubkey.serialize())
        , "/v", protocol_version
        , "/uri/");
}

std::string compute_uri_swarm_name(boost::string_view prefix, boost::string_view uri)
{
    return util::str(prefix, uri);
}

}} // namespaces
