#pragma once

#include "../util/crypto.h"

namespace ouinet { namespace cache {

class ChainHash {
public:
    using PrivateKey = util::Ed25519PrivateKey;
    using PublicKey  = util::Ed25519PublicKey;
    using Signature  = PublicKey::sig_array_t;
    using Hash       = util::SHA512;
    using Digest     = Hash::digest_type;

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
            Digest digest)
    {
        static const auto fmt_ = "%s%c%d%c%s";
        return ( boost::format(fmt_)
               % injection_id % '\0'
               % offset % '\0'
               % util::bytes::to_string_view(digest)).str();
    }
};

class ChainHasher {
public:
    using PrivateKey = ChainHash::PrivateKey;
    using Signature  = ChainHash::Signature;
    using Hash       = ChainHash::Hash;
    using Digest     = ChainHash::Digest;

    struct Signer {
        const std::string& injection_id;
        const PrivateKey&  key;

        Signature sign(size_t offset, const Digest& chained_digest) const {
            return key.sign(ChainHash::str_to_sign(injection_id, offset, chained_digest));
        }
    };

    using SigOrSigner = boost::variant<Signature, Signer>;

public:
    ChainHasher()
        : _offset(0)
    {}

    ChainHash calculate_block(size_t data_size, Digest data_digest, SigOrSigner sig_or_signer)
    {
        Hash chained_hasher;

        if (_prev_chained_signature) {
            chained_hasher.update(*_prev_chained_signature);
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

}} // namespaces
