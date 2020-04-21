#pragma once

#include "../logger.h"

namespace ouinet { namespace cache {

class SignedHead : public http_response::Head {
private:
    using Base = http_response::Head;

public:
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

public:
    SignedHead( const http::request_header<>& rqh
              , http::response_header<> rsh
              , const std::string& injection_id
              , std::chrono::seconds::rep injection_ts
              , const util::Ed25519PrivateKey& sk
              , const std::string& key_id)
        : Base(sign_response( rqh
                            , std::move(rsh)
                            , injection_id
                            , injection_ts
                            , sk
                            , key_id))
    {}

    static
    boost::optional<SignedHead> verify_and_create();

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
    //     X-Ouinet-Version: 2
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
                 , const util::Ed25519PrivateKey& sk
                 , const std::string& key_id);

    static
    http::response_header<>
    without_framing(const http::response_header<>& rsh)
    {
        http::response<http::empty_body> rs(rsh);
        rs.chunked(false);  // easier with a whole response
        rs.erase(http::field::content_length);  // 0 anyway because of empty body
        rs.erase(http::field::trailer);
        return rs.base();
    }

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
};

inline
http::response_header<>
SignedHead::sign_response( const http::request_header<>& rqh
                         , http::response_header<> rsh
                         , const std::string& injection_id
                         , std::chrono::seconds::rep injection_ts
                         , const util::Ed25519PrivateKey& sk
                         , const std::string& key_id)
{
    using namespace ouinet::http_;
    // TODO: This should be a `static_assert`.
    assert(protocol_version_hdr_current == protocol_version_hdr_v4);

    rsh.set(protocol_version_hdr, protocol_version_hdr_v4);
    rsh.set(response_uri_hdr, rqh.target());
    rsh.set(response_injection_hdr
           , boost::format("id=%s,ts=%d") % injection_id % injection_ts);
    static const auto fmt_ = "keyId=\"%s\""
                             ",algorithm=\"" + sig_alg_hs2019() + "\""
                             ",size=%d";
    rsh.set( response_block_signatures_hdr
           , boost::format(fmt_) % key_id % response_data_block);

    // Create a signature of the initial head.
    auto to_sign = without_framing(rsh);
    rsh.set(initial_signature_hdr(), http_signature(to_sign, sk, key_id, injection_ts));

    // Enabling chunking is easier with a whole respone,
    // and we do not care about content length anyway.
    http::response<http::empty_body> rs(std::move(rsh));
    rs.chunked(true);
    static const std::string trfmt_ = ( "%s%s"
                                      + response_data_size_hdr + ", Digest, "
                                      + final_signature_hdr());
    auto trfmt = boost::format(trfmt_);
    auto trhdr = rs[http::field::trailer];
    rs.set( http::field::trailer
          , (trfmt % trhdr % (trhdr.empty() ? "" : ", ")).str() );

    return rs.base();
}

inline
boost::optional<http::response_header<>>
SignedHead::verify(http::response_header<> rsh, const util::Ed25519PublicKey& pk)
{
    // Put together the head to be verified:
    // given head, minus chunking (and related headers), and signatures themselves.
    // Collect signatures found in the meanwhile.
    http::response_header<> to_verify, sig_headers;
    to_verify = SignedHead::without_framing(rsh);
    for (auto hit = rsh.begin(); hit != rsh.end();) {
        auto hn = hit->name_string();
        if (boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx)) {
            sig_headers.insert(hit->name(), hn, hit->value());
            to_verify.erase(hn);
            hit = rsh.erase(hit);  // will re-add at the end, minus bad signatures
        } else hit++;
    }

    auto keyId = http_key_id_for_injection(pk);  // TODO: cache this
    bool sig_ok = false;
    http::fields extra = rsh;  // all extra for the moment

    // Go over signature headers: parse, select, verify.
    int sig_idx = 0;
    auto keep_signature = [&] (const auto& sig) {
        rsh.insert(http_::response_signature_hdr_pfx + std::to_string(sig_idx++), sig);
    };
    for (auto& hdr : sig_headers) {
        auto hn = hdr.name_string();
        auto hv = hdr.value();
        auto sig = HttpSignature::parse(hv);
        if (!sig) {
            LOG_WARN("Malformed HTTP signature in header: ", hn);
            continue;  // drop signature
        }
        if (sig->keyId != keyId) {
            LOG_DEBUG("Unknown key for HTTP signature in header: ", hn);
            keep_signature(hv);
            continue;
        }
        if (!(sig->algorithm.empty()) && sig->algorithm != SignedHead::sig_alg_hs2019()) {
            LOG_WARN( "Unsupported algorithm \"", sig->algorithm
                    , "\" for HTTP signature in header: ", hn);
            continue;  // drop signature
        }
        auto ret = sig->verify(to_verify, pk);
        if (!ret.first) {
            LOG_WARN("Head does not match HTTP signature in header: ", hn);
            continue;  // drop signature
        }
        LOG_DEBUG("Head matches HTTP signature: ", hn);
        sig_ok = true;
        keep_signature(hv);
        for (auto ehit = extra.begin(); ehit != extra.end();)  // note extra headers
            if (ret.second.find(ehit->name_string()) == ret.second.end())
                ehit = extra.erase(ehit);  // no longer an extra header
            else
                ehit++;  // still an extra header
    }

    if (!sig_ok)
        return boost::none;

    for (auto& eh : extra) {
        LOG_WARN("Dropping header not in HTTP signatures: ", eh.name_string());
        rsh.erase(eh.name_string());
    }
    return rsh;
}

}} // namespace
