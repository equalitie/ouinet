#include "hash_list.h"
#include "http_sign.h"

using namespace ouinet;
using namespace ouinet::cache;

using boost::optional;

bool HashList::verify() const {
    using Digest = util::SHA512::digest_type;

    optional<Digest> last_digest;

    size_t block_size = signed_head.block_size();
    size_t last_offset = 0;

    bool first = true;

    for (auto& digest : block_hashes) {
        util::SHA512 sha;

        if (last_digest) {
            sha.update(*last_digest);
        }

        sha.update(digest);

        last_digest = sha.close();

        if (first) {
            first = false;
        } else {
            last_offset += block_size;
        }
    }

    if (!last_digest) return false;


    return cache::Block::verify( signed_head.injection_id()
                               , last_offset
                               , *last_digest
                               , signature
                               , signed_head.public_key());
}

