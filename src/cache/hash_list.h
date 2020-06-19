#pragma once

#include "../util/hash.h"
#include "../util/crypto.h"
#include "../response_part.h"
#include "signed_head.h"

namespace ouinet { namespace cache {

struct HashList {
    using Digest = util::SHA512::digest_type;
    using PubKey = util::Ed25519PublicKey;

    SignedHead          signed_head;
    PubKey::sig_array_t signature;
    std::vector<Digest> block_hashes;

    bool verify() const;

    static HashList load(
            http_response::AbstractReader&,
            const PubKey&,
            Cancel&,
            asio::yield_context);
};

}}
