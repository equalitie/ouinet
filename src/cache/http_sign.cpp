#include "http_sign.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/format.hpp>

#include "../constants.h"
#include "../parse/number.h"
#include "../split_string.h"
#include "../util.h"
#include "../util/bytes.h"
#include "../util/hash.h"

namespace ouinet { namespace cache {

static const auto initial_signature_hdr = http_::response_signature_hdr_pfx + "0";
static const auto final_signature_hdr = http_::response_signature_hdr_pfx + "1";

// The only signature algorithm supported by this implementation.
static const std::string sig_alg_hs2019("hs2019");

static const std::string key_id_pfx("ed25519=");

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

http::response_header<>
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , std::chrono::seconds::rep injection_ts
                   , const util::Ed25519PrivateKey& sk
                   , const std::string& key_id)
{
    using namespace ouinet::http_;
    assert(response_version_hdr_current == response_version_hdr_v0);

    rsh.set(response_version_hdr, response_version_hdr_v0);
    rsh.set(response_uri_hdr, rqh.target());
    rsh.set(response_injection_hdr
           , boost::format("id=%s,ts=%d") % injection_id % injection_ts);
    static const auto fmt_ = "keyId=\"%s\""
                             ",algorithm=\"" + sig_alg_hs2019 + "\""
                             ",size=%d";
    rsh.set( response_block_signatures_hdr
           , boost::format(fmt_) % key_id % response_data_block);

    // Create a signature of the initial head.
    auto to_sign = without_framing(rsh);
    rsh.set(initial_signature_hdr, http_signature(to_sign, sk, key_id, injection_ts));

    // Enabling chunking is easier with a whole respone,
    // and we do not care about content length anyway.
    http::response<http::empty_body> rs(std::move(rsh));
    rs.chunked(true);
    static const std::string trfmt_ = ( "%s%s"
                                      + response_data_size_hdr + ", Digest, "
                                      + final_signature_hdr);
    auto trfmt = boost::format(trfmt_);
    auto trhdr = rs[http::field::trailer];
    rs.set( http::field::trailer
          , (trfmt % trhdr % (trhdr.empty() ? "" : ", ")).str() );

    return rs.base();
}

boost::string_view
http_sign_detail::get_injection_id(const http::response_header<>& rsh)
{
    auto ih = rsh[http_::response_injection_hdr];
    if (ih.empty()) return {};  // missing header
    for (auto item : SplitString(ih, ',')) {
        auto k_v = split_string_pair(item, '=');
        if (k_v.first != "id") continue;
        return k_v.second;
    }
    return {};  // missing id item in header
}

http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const util::SHA256::digest_type& content_digest
                      , const util::Ed25519PrivateKey& sk
                      , const std::string& key_id
                      , std::chrono::seconds::rep ts)
{
    using namespace ouinet::http_;
    // Pending trailer headers to support the signature.
    rst.set(response_data_size_hdr, content_length);
    rst.set(http::field::digest, "SHA-256=" + util::base64_encode(content_digest));

    // Put together the head to be signed:
    // initial head, minus chunking (and related headers) and its signature,
    // plus trailer headers.
    // Use `...-Data-Size` internal header instead on `Content-Length`.
    auto to_sign = without_framing(rsh);
    to_sign.erase(initial_signature_hdr);
    for (auto& hdr : rst)
        to_sign.set(hdr.name_string(), hdr.value());

    rst.set(final_signature_hdr, http_signature(to_sign, sk, key_id, ts));
    return rst;
}

http::response_header<>
http_injection_verify( http::response_header<> rsh
                     , const util::Ed25519PublicKey& pk)
{
    // Put together the head to be verified:
    // given head, minus chunking (and related headers), and signatures themselves.
    // Collect signatures found in the meanwhile.
    http::response_header<> to_verify, sig_headers;
    to_verify = without_framing(rsh);
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
        if (!(sig->algorithm.empty()) && sig->algorithm != sig_alg_hs2019) {
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
        return {};

    for (auto& eh : extra) {
        LOG_WARN("Dropping header not in HTTP signatures: ", eh.name_string());
        rsh.erase(eh.name_string());
    }
    return rsh;
}

std::string
http_key_id_for_injection(const util::Ed25519PublicKey& pk)
{
    return key_id_pfx + util::base64_encode(pk.serialize());
}

boost::optional<util::Ed25519PublicKey>
http_decode_key_id(boost::string_view key_id)
{
    if (!key_id.starts_with(key_id_pfx)) return {};
    auto decoded_pk = util::base64_decode(key_id.substr(key_id_pfx.size()));
    if (decoded_pk.size() != util::Ed25519PublicKey::key_size) return {};
    auto pk_array = util::bytes::to_array<uint8_t, util::Ed25519PrivateKey::key_size>(decoded_pk);
    return util::Ed25519PublicKey(std::move(pk_array));
}

boost::optional<util::Ed25519PublicKey::sig_array_t>
http_sign_detail::block_sig_from_exts(boost::string_view xs)
{
    if (xs.empty()) return {};  // no extensions

    sys::error_code ec;
    http::chunk_extensions xp;
    xp.parse(xs, ec);
    assert(!ec);  // this should have been validated upstream, fail hard otherwise

    auto xit = std::find_if( xp.begin(), xp.end()
                           , [](const auto& x) {
                                 return x.first == http_::response_block_signature_ext;
                             });
    if (xit == xp.end()) return {};  // no signature

    auto decoded_sig = util::base64_decode(xit->second);
    if (decoded_sig.size() != util::Ed25519PublicKey::sig_size) {
        LOG_WARN("Malformed data block signature");
        return {};  // invalid Base64, invalid length
    }

    return util::bytes::to_array<uint8_t, util::Ed25519PublicKey::sig_size>(decoded_sig);
}

std::string
http_sign_detail::block_sig_str_pfx( const std::string& injection_id
                                   , size_t offset)
{
    static const auto fmt_ = "%s%c%d%c";
    return ( boost::format(fmt_)
           % injection_id % '\0'
           % offset % '\0').str();
}

static inline
std::string
block_chunk_ext_(const util::Ed25519PublicKey::sig_array_t& sig)
{
    static const auto fmt_ = ";" + http_::response_block_signature_ext + "=\"%s\"";
    auto encoded_sig = util::base64_encode(sig);
    return (boost::format(fmt_) % encoded_sig).str();
}

std::string
http_sign_detail::block_chunk_ext( const std::string& sig_str_pfx
                                 , util::SHA512& hash
                                 , const util::Ed25519PrivateKey& sk)
{
    auto digest = util::bytes::to_string(hash.close());
    return block_chunk_ext_(sk.sign(sig_str_pfx + digest));
}

std::string
http_sign_detail::block_chunk_ext(const boost::optional<util::Ed25519PublicKey::sig_array_t>& s)
{
    if (!s) return {};
    return block_chunk_ext_(*s);
}

bool
http_sign_detail::check_body( const http::response_header<>& head
                            , size_t body_length
                            , util::SHA256& body_hash)
{
    // Check body length.
    auto h_body_length_h = head[http_::response_data_size_hdr];
    auto h_body_length = parse::number<size_t>(h_body_length_h);
    if (h_body_length) {
        if (*h_body_length != body_length) {
            LOG_WARN("Body length mismatch: ", *h_body_length, "!=", body_length);
            return false;
        }
        LOG_DEBUG("Body matches signed length: ", body_length);
    }

    // Get body digest value.
    auto b_digest = http_digest(body_hash);
    auto b_digest_s = split_string_pair(b_digest, '=');

    // Get digest values in head and compare (if algorithm matches).
    auto h_digests = head.equal_range(http::field::digest);
    for (auto hit = h_digests.first; hit != h_digests.second; hit++) {
        auto h_digest_s = split_string_pair(hit->value(), '=');
        if (boost::algorithm::iequals(b_digest_s.first, h_digest_s.first)) {
            if (b_digest_s.second != h_digest_s.second) {
                LOG_WARN("Body digest mismatch: ", hit->value(), "!=", b_digest);
                return false;
            }
            LOG_DEBUG("Body matches signed digest: ", b_digest);
        }
    }

    return true;
}

std::string
http_digest(util::SHA256& hash)
{
    auto digest = hash.close();
    auto encoded_digest = util::base64_encode(digest);
    return "SHA-256=" + encoded_digest;
}

std::string
http_digest(const http::response<http::dynamic_body>& rs)
{
    util::SHA256 hash;

    // Feed each buffer of body data into the hash.
    for (auto it : rs.body().data())
        hash.update(it);

    return http_digest(hash);
}

template<class Head>
static void
prep_sig_head(const Head& inh, Head& outh)
{
    using namespace std;

    // Lowercase header names, to more-or-less respect input order.
    vector<string> hdr_sorted;
    // Lowercase header name to `, `-concatenated, trimmed values.
    map<string, string> hdr_values;

    for (auto& hdr : inh) {
        auto name = hdr.name_string().to_string();  // lowercase
        boost::algorithm::to_lower(name);

        auto value_v = hdr.value();  // trimmed
        trim_whitespace(value_v);

        auto vit = hdr_values.find(name);
        if (vit == hdr_values.end()) {  // new entry, add
            hdr_values[name] = value_v.to_string();
            hdr_sorted.push_back(name);
        } else {  // existing entry, concatenate
            vit->second += ", ";
            vit->second.append(value_v.data(), value_v.length());
        }
    }

    for (auto name : hdr_sorted)
        outh.set(name, hdr_values[name]);
}

static inline std::string
request_target_ph(const http::request_header<>& rqh)
{
    auto method = rqh.method_string().to_string();
    boost::algorithm::to_lower(method);
    return util::str(method, ' ', rqh.target());
}

static inline std::string
request_target_ph(const http::response_header<>&)
{
    return {};
}

static inline std::string
response_status_ph(const http::request_header<>&)
{
    return {};
}

static inline std::string
response_status_ph(const http::response_header<>& rsh)
{
    return std::to_string(rsh.result_int());
}

// For `hn` being ``X-Foo``, turn:
//
//     X-Foo: foo
//     X-Bar: xxx
//     X-Foo: 
//     X-Foo: bar
//
// into optional ``foo, , bar``, and:
//
//     X-Bar: xxx
//
// into optional no value.
template<class Head>
static
boost::optional<std::string>
flatten_header_values(const Head& inh, const boost::string_view& hn)
{
    typename Head::const_iterator begin, end;
    std::tie(begin, end) = inh.equal_range(hn);
    if (begin == inh.end())  // missing header
        return {};

    std::string ret;
    for (auto hit = begin; hit != end; hit++) {
        auto hv = hit->value();
        trim_whitespace(hv);
        if (!ret.empty()) ret += ", ";
        ret.append(hv.data(), hv.size());
    }
    return {std::move(ret)};
}

template<class Head>
static boost::optional<Head>
verification_head(const Head& inh, const HttpSignature& hsig)
{
    Head vh;
    for (const auto& hn : SplitString(hsig.headers, ' ')) {
        // A listed header missing in `inh` is considered an error,
        // thus the verification should fail.
        if (hn[0] != '(') {  // normal headers
            // Referring to an empty header is ok (a missing one is not).
            auto hcv = flatten_header_values(inh, hn);
            if (!hcv) return {};
            vh.set(hn, *hcv);
        } else if (hn == "(request-target)") {  // pseudo-headers
            auto hv = request_target_ph(inh);
            if (hv.empty()) return {};
            vh.set(hn, std::move(hv));
        } else if (hn == "(response-status)") {
            auto hv = response_status_ph(inh);
            if (hv.empty()) return {};
            vh.set(hn, std::move(hv));
        } else if (hn == "(created)") {
            vh.set(hn, hsig.created);
        } else if (hn == "(expires)") {
            vh.set(hn, hsig.expires);
        } else {
            LOG_WARN("Unknown HTTP signature pseudo-header: ", hn);
            return {};
        }
    }
    return {std::move(vh)};
}

template<class Head>
static std::pair<std::string, std::string>
get_sig_str_hdrs(const Head& sig_head)
{
    std::string sig_string, headers;
    bool ins_sep = false;
    for (auto& hdr : sig_head) {
        auto name = hdr.name_string();
        auto value = hdr.value();

        if (ins_sep) sig_string += '\n';
        sig_string += (boost::format("%s: %s") % name % value).str();

        if (ins_sep) headers += ' ';
        headers.append(name.data(), name.length());

        ins_sep = true;
    }

    return {std::move(sig_string), std::move(headers)};
}

std::string
http_signature( const http::response_header<>& rsh
              , const util::Ed25519PrivateKey& sk
              , const std::string& key_id
              , std::chrono::seconds::rep ts)
{
    static const auto fmt_ = "keyId=\"%s\""
                             ",algorithm=\"" + sig_alg_hs2019 + "\""
                             ",created=%d"
                             ",headers=\"%s\""
                             ",signature=\"%s\"";
    boost::format fmt(fmt_);

    http::response_header<> sig_head;
    sig_head.set("(response-status)", rsh.result_int());
    sig_head.set("(created)", ts);
    prep_sig_head(rsh, sig_head);  // unique fields, lowercase names, trimmed values

    std::string sig_string, headers;
    std::tie(sig_string, headers) = get_sig_str_hdrs(sig_head);

    auto encoded_sig = util::base64_encode(sk.sign(sig_string));

    return (fmt % key_id % ts % headers % encoded_sig).str();
}

static bool
has_comma_in_quotes(const boost::string_view& s) {
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

boost::optional<HttpBlockSigs>
HttpBlockSigs::parse(boost::string_view bsigs)
{
    // TODO: proper support for quoted strings
    if (has_comma_in_quotes(bsigs)) {
        LOG_WARN("Commas in quoted arguments of block signatures HTTP header are not yet supported");
        return {};
    }

    HttpBlockSigs hbs;
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
            auto pk = http_decode_key_id(value);
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
    if (hbs.algorithm != sig_alg_hs2019) {
        LOG_WARN("Missing or invalid algorithm in block signatures HTTP header");
        return {};
    }
    if (hbs.size == 0) {
        LOG_WARN("Missing or invalid size in block signatures HTTP header");
        return {};
    }
    return hbs;
}

boost::optional<HttpSignature>
HttpSignature::parse(boost::string_view sig)
{
    // TODO: proper support for quoted strings
    if (has_comma_in_quotes(sig)) {
        LOG_WARN("Commas in quoted arguments of HTTP signatures are not yet supported");
        return {};
    }

    HttpSignature hs;
    static const std::string def_headers = "(created)";
    hs.headers = def_headers;  // missing is not the same as empty

    for (boost::string_view item : SplitString(sig, ',')) {
        beast::string_view key, value;
        std::tie(key, value) = split_string_pair(item, '=');
        // Unquoted values:
        if (key == "created") {hs.created = value; continue;}
        if (key == "expires") {hs.expires = value; continue;}
        // Quoted values:
        if (value.size() < 2 || value[0] != '"' || value[value.size() - 1] != '"')
            return {};
        value.remove_prefix(1);
        value.remove_suffix(1);
        if (key == "keyId") {hs.keyId = value; continue;}
        if (key == "algorithm") {hs.algorithm = value; continue;}
        if (key == "headers") {hs.headers = value; continue;}
        if (key == "signature") {hs.signature = value; continue;}
        return {};
    }
    if (hs.keyId.empty() || hs.signature.empty()) {  // required
        LOG_WARN("HTTP signature contains empty key identifier or signature");
        return {};
    }
    if (hs.algorithm.empty() || hs.created.empty() || hs.headers.empty()) {  // recommended
        LOG_WARN("HTTP signature contains empty algorithm, creation time stamp, or header list");
    }

    return {std::move(hs)};
}

std::pair<bool, http::fields>
HttpSignature::verify( const http::response_header<>& rsh
                     , const util::Ed25519PublicKey& pk)
{
    // The key may imply an algorithm,
    // but an explicit algorithm should not conflict with the key.
    assert(algorithm.empty() || algorithm == sig_alg_hs2019);

    auto vfy_head = verification_head(rsh, *this);
    if (!vfy_head)  // e.g. because of missing headers
        return {false, {}};

    std::string sig_string;
    std::tie(sig_string, std::ignore) = get_sig_str_hdrs(*vfy_head);

    auto decoded_sig = util::base64_decode(signature);
    if (decoded_sig.size() != pk.sig_size) {
        LOG_WARN( "Invalid HTTP signature length: "
                , decoded_sig.size(), " != ", static_cast<size_t>(pk.sig_size)
                , " ", signature);
        return {false, {}};
    }

    auto sig_array = util::bytes::to_array<uint8_t, util::Ed25519PublicKey::sig_size>(decoded_sig);
    if (!pk.verify(sig_string, sig_array))
        return {false, {}};

    // Collect headers not covered by signature.
    http::fields extra;
    for (const auto& hdr : rsh) {
        auto hn = hdr.name_string();
        if (vfy_head->find(hn) == vfy_head->end())
            extra.insert(hdr.name(), hn, hdr.value());
    }

    return {true, std::move(extra)};
}

}} // namespaces
