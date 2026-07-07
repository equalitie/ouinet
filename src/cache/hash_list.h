#pragma once
#include <cstdint>

#include "../util/hash.h"
#include "../util/sign.h"
#include "../response_part.h"
#include "../api.h"
#include "signed_head.h"

namespace ouinet {
    class Async;
}

namespace ouinet::cache {

struct OUINET_CLIENT_API HashList {
    using Digest    = util::SHA512::digest_type;
    using PubKey    = sign::PublicKey;

    struct Block {
        Digest data_hash;
        sign::Signature chained_hash_signature;
    };

    SignedHead         signed_head;
    std::vector<Block> blocks;

    bool verify() const;

    static HashList load(
            http_response::Reader&,
            const PubKey&,
            Cancel&,
            asio::yield_context);

    [[nodiscard]]
    std::expected<void, sys::error_code>
    write(GenericStream&, Async) const;

    boost::optional<Block> get_block(size_t block_id) const
    {
        if (block_id >= blocks.size()) {
            return boost::none;
        }
        return blocks[block_id];
    }

};

} // namespace ouinet::cache
