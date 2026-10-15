#include "chain_hasher.h"
#include <boost/format.hpp>

namespace ouinet::cache {

/* static */
std::string ChainHash::str_to_sign(
        const std::string& injection_id,
        size_t offset,
        Digest digest)
{
    static const auto fmt_ = "%s%c%d%c%s";
    return ( boost::format(fmt_)
           % injection_id % '\0'
           % offset % '\0'
           % util::bytes::to_string_view(digest)).str();
}

ChainHash ChainHasher::calculate_block(size_t data_size, Digest data_digest, ChainHasher::SigOrSigner sig_or_signer)
{
    Hash chained_hasher;

    if (_prev_chained_signature) {
        chained_hasher.update(_prev_chained_signature->bytes);
    }

    if (_prev_chained_digest) {
        chained_hasher.update(*_prev_chained_digest);
    }

    chained_hasher.update(data_digest);

    Digest chained_digest = chained_hasher.close();

    Signature chained_signature = util::apply(sig_or_signer,
            [&] (const Signature& s) { return s; },
            [&] (const Signer& s)    { return s.sign(_offset, chained_digest); });

    size_t old_offset = _offset;

    // Prepare for next block
    _offset += data_size;
    _prev_chained_digest    = chained_digest;
    _prev_chained_signature = chained_signature;

    return {old_offset, chained_digest, chained_signature};
}

} // namespace
