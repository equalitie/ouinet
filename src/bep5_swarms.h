#pragma once

#include "util/crypto.h"

namespace ouinet { namespace bep5 {

// https://redmine.equalit.ie/issues/14920#note-1
std::string compute_injector_swarm_name(const ouinet::util::Ed25519PublicKey&, unsigned protocol_version);
std::string compute_bridge_swarm_name(const ouinet::util::Ed25519PublicKey&, unsigned protocol_version);
std::string compute_uri_swarm_name(const ouinet::util::Ed25519PublicKey&, unsigned protocol_version, boost::string_view key);

}} // namespaces

