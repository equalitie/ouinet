#include "bep5_swarms.h"
#include "util/str.h"
#include "util/bytes.h"

namespace ouinet { namespace bep5 {

std::string compute_injector_swarm_name(const ouinet::util::Ed25519PublicKey& pubkey, unsigned protocol_version)
{
    return util::str
        ( "ed25519:", util::bytes::to_hex(pubkey.serialize())
        , "/v", protocol_version
        , "/injectors");
}

std::string compute_bridge_swarm_name(const ouinet::util::Ed25519PublicKey& pubkey, unsigned protocol_version)
{
    return util::str
        ( "ed25519:", util::bytes::to_hex(pubkey.serialize())
        , "/v", protocol_version
        , "/bridges");
}

std::string compute_uri_swarm_name(const ouinet::util::Ed25519PublicKey& pubkey, unsigned protocol_version, boost::string_view key)
{
    return util::str
        ( "ed25519:", util::bytes::to_hex(pubkey.serialize())
        , "/v", protocol_version
        , "/uri/", key);
}

}} // namespaces
