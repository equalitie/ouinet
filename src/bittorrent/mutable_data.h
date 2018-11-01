#pragma once

#include <chrono>
#include <deque>
#include <string>

#include "bencoding.h"
#include "node_id.h"

#include "../util/crypto.h"
#include "../util/signal.h"

namespace ouinet {
namespace bittorrent {

struct MutableDataItem {
    util::Ed25519PublicKey public_key;
    std::string salt;
    BencodedValue value;
    int64_t sequence_number;
    std::array<uint8_t, 64> signature;

    // Throws `std::length_error` if the value is too big.
    static MutableDataItem sign(
        BencodedValue value,
        int64_t sequence_number,
        boost::string_view salt,
        util::Ed25519PrivateKey private_key
    );

    bool verify() const;
};

} // bittorrent namespace
} // ouinet namespace
