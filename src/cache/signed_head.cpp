#include "signed_head.h"
#include "../logger.h"
#include "../http_util.h"
#include "../split_string.h"
#include "../parse/number.h"
#include "../util/bytes.h"

namespace ouinet::cache {

http::response_header<>
SignedHead::sign_response( const http::request_header<>& rqh
                         , http::response_header<> rsh
                         , const std::string& injection_id
                         , std::chrono::seconds::rep injection_ts
                         , const util::Ed25519PrivateKey& sk)
{
    using namespace ouinet::http_;

    auto pk = sk.public_key();
    auto key_id = encode_key_id(pk);

    // TODO: This should be a `static_assert`.
    assert(protocol_version_hdr_current == protocol_version_hdr_v6);

    rsh.set(protocol_version_hdr, protocol_version_hdr_v6);
    rsh.set(response_uri_hdr, rqh.target());
    rsh.set(response_injection_hdr
           , str(boost::format("id=%s,ts=%d") % injection_id % injection_ts));
    static const auto fmt_ = "keyId=\"%s\""
                             ",algorithm=\"" + sig_alg_hs2019() + "\""
                             ",size=%d";
    rsh.set( response_block_signatures_hdr
           , str(boost::format(fmt_) % key_id % response_data_block));

    // Create a signature of the initial head.
    auto to_sign = util::without_framing(rsh);
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

boost::optional<http::response_header<>>
SignedHead::verify(http::response_header<> rsh, const util::Ed25519PublicKey& pk)
{
    // Put together the head to be verified:
    // given head, minus chunking (and related headers), and signatures themselves.
    // Collect signatures found in the meanwhile.
    http::response_header<> to_verify, sig_headers;
    to_verify = util::without_framing(rsh);
    for (auto hit = rsh.begin(); hit != rsh.end();) {
        auto hn = hit->name_string();
        if (boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx)) {
            sig_headers.insert(hit->name(), hn, hit->value());
            to_verify.erase(hn);
            hit = rsh.erase(hit);  // will re-add at the end, minus bad signatures
        } else hit++;
    }

    auto keyId = encode_key_id(pk);  // TODO: cache this
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

boost::optional<SignedHead>
SignedHead::verify_and_create(http::response_header<> rsh, const util::Ed25519PublicKey& pk)
{
    auto rsh_o = verify(std::move(rsh), pk);
    if (!rsh_o) return boost::none;
    return create_from_trusted_source(std::move(*rsh_o));
}

boost::optional<SignedHead>
SignedHead::create_from_trusted_source(http::response_header<> rsh)
{
    auto uri = std::string(rsh[http_::response_uri_hdr]);
    // Get and validate HTTP block signature parameters.
    auto bsh = rsh[http_::response_block_signatures_hdr];
    if (bsh.empty()) {
        LOG_WARN("Missing parameters for HTTP data block signatures; uri=", uri);
        return boost::none;
    }
    auto bs_params = cache::SignedHead::BlockSigs::parse(bsh);
    if (!bs_params) {
        LOG_WARN("Malformed parameters for HTTP data block signatures; uri=", uri);
        return boost::none;
    }
    if (bs_params->size > http_::response_data_block_max) {
        LOG_WARN("Size of signed HTTP data blocks is too large: ", bs_params->size, "; uri=", uri);
        return boost::none;
    }
    // The injection id is also needed to verify block signatures.
    std::string injection_id{util::http_injection_id(rsh)};

    if (injection_id.empty()) {
        LOG_WARN("Missing injection identifier in HTTP head; uri=", uri);
        return boost::none;
    }

    auto tsh = util::http_injection_ts(rsh);
    auto injection_ts = parse::number<time_t>(tsh);

    if (!injection_ts) {
        LOG_WARN("Failed to parse injection time stamp \"", tsh, "\"");
        return boost::none;
    }

    return SignedHead( std::move(rsh)
                     , std::move(injection_id)
                     , *injection_ts
                     , std::move(uri)
                     , std::move(*bs_params));
}

boost::optional<SignedHead::BlockSigs>
SignedHead::BlockSigs::parse(boost::string_view bsigs)
{
    // TODO: proper support for quoted strings
    if (has_comma_in_quotes(bsigs)) {
        LOG_WARN("Commas in quoted arguments of block signatures HTTP header are not yet supported");
        return {};
    }

    BlockSigs hbs;
    bool valid_pk = false;
    for (boost::string_view item : SplitString(bsigs, ',')) {
        beast::string_view key, value;
        std::tie(key, value) = split_string_pair(item, '=');
        // Unquoted values:
        if (key == "size") {
            auto sz = parse::number<size_t>(value);
            hbs.size = sz ? *sz : 0; continue;
        }
        // Quoted values:
        if (value.size() < 2 || value[0] != '"' || value[value.size() - 1] != '"') {
            LOG_WARN("Invalid quoting in block signatures HTTP header");
            return {};
        }
        value.remove_prefix(1);
        value.remove_suffix(1);
        if (key == "keyId") {
            auto pk = decode_key_id(value);
            if (!pk) continue;
            hbs.pk = *pk;
            valid_pk = true;
            continue;
        }
        if (key == "algorithm") {hbs.algorithm = value; continue;}
        return {};
    }
    if (!valid_pk) {
        LOG_WARN("Missing or invalid key identifier in block signatures HTTP header");
        return {};
    }
    if (hbs.algorithm != SignedHead::sig_alg_hs2019()) {
        LOG_WARN("Missing or invalid algorithm in block signatures HTTP header");
        return {};
    }
    if (hbs.size == 0) {
        LOG_WARN("Missing or invalid size in block signatures HTTP header");
        return {};
    }
    return hbs;
}

bool
SignedHead::has_comma_in_quotes(const boost::string_view& s) {
    // A comma is between quotes if
    // the number of quotes before it is odd.
    int quotes_seen = 0;
    for (auto c : s) {
        if (c == '"') {
            quotes_seen++;
            continue;
        }
        if ((c == ',') && (quotes_seen % 2 != 0))
            return true;
    }
    return false;
}


} // namespace
