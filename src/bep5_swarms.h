#pragma once

#include "util/crypto.h"

namespace ouinet { namespace bep5 {

// Ouinet swarm names are constructed following this pattern:
//
//     <KEY_TYPE>:<BASE32(INJECTOR_PUBKEY)>/v<VERSION>/<WHAT>
//
// `KEY_TYPE` indicates the type of injector key following.
// Currently `ed25519` is the only supported value.
//
// `BASE32(INJECTOR_KEY)` is the unpadded, lower case result of
// encoding the injector public key using Base32.
//
// `VERSION` is the Ouinet protocol version number (decimal).
//
// `WHAT` depends on the protocol version and
// the kind of information made available via the swarm.
// Currently supported values for v4 are:
//
//   - `injectors`: uTP endpoints for reaching injectors
//     with the given `INJECTOR_KEY`.
//
//   - `bridges`: uTP endpoints for reaching bridges to
//     injectors with the given `INJECTOR_KEY`.
//
//   - `uri/<URI>`: uTP endpoints for reaching clients keeping a cached copy of
//     the given `URI` signed with the given `INJECTOR_KEY`.
//
// Please bear in mind that BitTorrent DHT IDs are not the swarm names themselves,
// but their respective SHA1 digests.

std::string compute_injector_swarm_name( const ouinet::util::Ed25519PublicKey&
                                       , unsigned protocol_version);

std::string compute_bridge_swarm_name( const ouinet::util::Ed25519PublicKey&
                                     , unsigned protocol_version);

std::string compute_uri_swarm_name( const ouinet::util::Ed25519PublicKey&
                                  , unsigned protocol_version
                                  , boost::string_view key);

}} // namespaces

