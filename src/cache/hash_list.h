#pragma once
#include <cstdint>

#include "../util/hash.h"
#include "../util/crypto.h"
#include "../response_part.h"
#include "signed_head.h"

namespace ouinet { namespace cache {

struct HashList {
    using Digest    = util::SHA512::digest_type;
    using PubKey    = util::Ed25519PublicKey;
    using Signature = PubKey::sig_array_t;

    struct Block {
        Digest data_hash;
        Signature chained_hash_signature;
    };

    SignedHead         signed_head;
    std::vector<Block> blocks;

    bool verify() const;

    static HashList load(
            http_response::Reader&,
            const PubKey&,
            Cancel&,
            asio::yield_context);

    void write(GenericStream&, Cancel&, asio::yield_context) const;

    boost::optional<Block> get_block(size_t block_id) const
    {
        if (block_id >= blocks.size()) {
            return boost::none;
        }
        return blocks[block_id];
    }

};

}}
