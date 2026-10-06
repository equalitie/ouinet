#pragma once

#include "../util/sign.h"
#include "../util/bytes.h"
#include "../util/hash.h"
#include "../util/variant.h"

namespace ouinet::cache {

class ChainHash {
public:
    using SecretKey = sign::SecretKey;
    using PublicKey = sign::PublicKey;
    using Signature = sign::Signature;
    using Hash      = util::SHA512;
    using Digest    = Hash::digest_type;

    size_t    offset;
    Digest    chain_digest;
    Signature chain_signature;

    bool verify(const PublicKey& pk, const std::string& injection_id) const {
        return pk.verify(str_to_sign(injection_id, offset, chain_digest), chain_signature);
    }

private:
    friend class ChainHasher;

    static
    std::string str_to_sign(
            const std::string& injection_id,
            size_t offset,
            Digest digest);
};

class ChainHasher {
public:
    using SecretKey = ChainHash::SecretKey;
    using Signature = ChainHash::Signature;
    using Hash      = ChainHash::Hash;
    using Digest    = ChainHash::Digest;

    struct Signer {
        const std::string& injection_id;
        const SecretKey&  key;

        Signature sign(size_t offset, const Digest& chained_digest) const {
            return key.sign(ChainHash::str_to_sign(injection_id, offset, chained_digest));
        }
    };

    using SigOrSigner = boost::variant<Signature, Signer>;

public:
    ChainHasher()
        : _offset(0)
    {}

    ChainHash calculate_block(size_t data_size, Digest data_digest, SigOrSigner sig_or_signer);

    void set_prev_chained_digest(Digest prev_chained_digest) {
        _prev_chained_digest = prev_chained_digest;
    }

    void set_offset(size_t offset) {
        _offset = offset;
    }

    const boost::optional<Digest>& prev_chained_digest() const {
        return _prev_chained_digest;
    }

private:
    size_t _offset;
    boost::optional<Digest> _prev_chained_digest;
    boost::optional<Signature> _prev_chained_signature;
};

} // namespace
