#pragma once

#include "http_sign.h"
#include "util.h"

namespace ouinet::cache {

class SignedHead : public http_response::Head {
private:
    using Base = http_response::Head;

public:
    // A simple container for a parsed block signatures HTTP header.
    // Only the `hs2019` algorithm with an explicit key is supported,
    // so the ready-to-use key is left in `pk`.
    struct BlockSigs {
        util::Ed25519PublicKey pk;
        boost::string_view algorithm;  // always "hs2019"
        size_t size;
    
        static
        boost::optional<BlockSigs> parse(boost::string_view);
    };

    // The only signature algorithm supported by this implementation.
    static const std::string& sig_alg_hs2019() {
        static std::string ret = "hs2019";
        return ret;
    }

    static const std::string& initial_signature_hdr() {
        static std::string ret = http_::response_signature_hdr_pfx + "0";
        return ret;
    }

    static const std::string& final_signature_hdr() {
        static std::string ret = http_::response_signature_hdr_pfx + "1";
        return ret;
    }

    static const std::string& key_id_pfx() {
        static std::string ret = "ed25519=";
        return ret;
    }

public:
    SignedHead() {}

    SignedHead( const http::request_header<>& rqh
              , http::response_header<> rsh
              , const std::string& injection_id
              , std::chrono::seconds::rep injection_ts
              , const util::Ed25519PrivateKey& sk)
        : Base(sign_response( rqh
                            , std::move(rsh)
                            , injection_id
                            , injection_ts
                            , sk))
        , _injection_id(injection_id)
        , _injection_ts(injection_ts)
        , _uri(rqh.target())
        , _bs_params{ sk.public_key()
                    , sig_alg_hs2019()
                    , http_::response_data_block}
    {}

private:
    SignedHead( http::response_header<> signed_rsh
              , std::string injection_id
              , std::chrono::seconds::rep injection_ts
              , std::string uri
              , SignedHead::BlockSigs bs_params)
        : Base(std::move(signed_rsh))
        , _injection_id(std::move(injection_id))
        , _injection_ts(injection_ts)
        , _uri(std::move(uri))
        , _bs_params(std::move(bs_params))
    {}

public:
    static
    boost::optional<SignedHead>
    verify_and_create(http::response_header<>, const util::Ed25519PublicKey&);

    static
    boost::optional<SignedHead>
    create_from_trusted_source(http::response_header<>);

    // Ouinet-specific declarations for injection using HTTP signatures
    // ----------------------------------------------------------------
    
    // Get an extended version of the given response head
    // with an additional signature header and
    // other headers required to support that signature and
    // a future one for the full message head (as part of the trailer).
    //
    // Example:
    //
    //     ...
    //     X-Ouinet-Version: 6
    //     X-Ouinet-URI: https://example.com/foo
    //     X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
    //     X-Ouinet-BSigs: keyId="...",algorithm="hs2019",size=65536
    //     X-Ouinet-Sig0: keyId="...",algorithm="hs2019",created=1516048310,
    //       headers="(response-status) (created) ... x-ouinet-injection",
    //       signature="..."
    //     Transfer-Encoding: chunked
    //     Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1
    //
    static
    http::response_header<>
    sign_response( const http::request_header<>& rqh
                 , http::response_header<> rsh
                 , const std::string& injection_id
                 , std::chrono::seconds::rep injection_ts
                 , const util::Ed25519PrivateKey& sk);

    // Verify that the given response head contains
    // good signatures for it from the given public key.
    // Return a head which only contains headers covered by at least one such signature,
    // plus good signatures themselves and signatures for unknown keys.
    // Bad signatures are dropped to avoid propagating them along good signatures.
    // Framing headers are preserved.
    //
    // If no good signatures exist, or any other error happens,
    // return an empty head.
    static
    boost::optional<http::response_header<>>
    verify(http::response_header<>, const ouinet::util::Ed25519PublicKey&);

    static
    bool
    has_comma_in_quotes(const boost::string_view& s);

    const std::string& injection_id() const { return _injection_id; }
    const std::string& uri()          const { return _uri; }
    size_t block_size()               const { return _bs_params.size; }

    const util::Ed25519PublicKey& public_key() const { return _bs_params.pk; }

    static std::string encode_key_id(const util::Ed25519PublicKey& pk) {
        return key_id_pfx() + util::base64_encode(pk.serialize());
    }

    std::string encode_key_id() const {
        return encode_key_id(public_key());
    }

    bool more_recent_than(const SignedHead& other) const {
        return _injection_ts > other._injection_ts;
    }

private:
    static
    boost::optional<util::Ed25519PublicKey>
    decode_key_id(boost::string_view key_id)
    {
        using PublicKey = util::Ed25519PublicKey::key_array_t;

        if (!key_id.starts_with(key_id_pfx())) return {};
        auto decoded_pk = util::base64_decode<PublicKey>(key_id.substr(key_id_pfx().size()));
        if (!decoded_pk) return {};
        return util::Ed25519PublicKey(*decoded_pk);
    }

private:
    std::string _injection_id;
    std::chrono::seconds::rep _injection_ts;
    std::string _uri;
    SignedHead::BlockSigs _bs_params;
};

} // namespace
