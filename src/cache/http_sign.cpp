#include "http_sign.h"
#include "signed_head.h"

#include <algorithm>
#include <map>
#include <queue>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/format.hpp>
#include <boost/system/error_code.hpp>

#include "chain_hasher.h"
#include "../constants.h"
#include "../http_util.h"
#include "../logger.h"
#include "../parse/number.h"
#include "../split_string.h"
#include "../util.h"
#include "../util/bytes.h"
#include "../util/hash.h"
#include "../util/quantized_buffer.h"
#include "../util/variant.h"

namespace ouinet { namespace cache {

using sig_array_t = util::Ed25519PublicKey::sig_array_t;
using block_digest_t = util::SHA512::digest_type;
using opt_sig_array_t = boost::optional<sig_array_t>;
using opt_block_digest_t = boost::optional<block_digest_t>;

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
    auto to_sign = SignedHead::without_framing(rsh);
    to_sign.erase(SignedHead::initial_signature_hdr());
    for (auto& hdr : rst)
        to_sign.set(hdr.name_string(), hdr.value());

    rst.set(SignedHead::final_signature_hdr(), http_signature(to_sign, sk, key_id, ts));
    return rst;
}

static inline
std::set<SplitString::value_type>
sig_headers_set(const boost::string_view& headers)
{
    auto hs = SplitString(headers, ' ');
    return {hs.begin(), hs.end()};
}

template<class Set>
static
bool
has_extra_items(const Set& s1, const Set& s2)
{
    for (const auto& s1item : s1)
        if (s2.find(s1item) == s2.end())
            return true;
    return false;
}

static
void
insert_trailer(const http::fields::value_type& th, http::response_header<>& head)
{
    auto thn = th.name_string();
    auto thv = th.value();
    if (!boost::regex_match(thn.begin(), thn.end(), http_::response_signature_hdr_rx)) {
        head.insert(th.name(), thn, thv);
        return;
    }

    // Signature, look for redundant signatures in head.
    auto thsig = HttpSignature::parse(thv);
    assert(thsig);
    auto ths_hdrs = sig_headers_set(thsig->headers);
    auto ths_ts = parse::number<time_t>(thsig->created);
    if (!ths_ts) {
        LOG_WARN( "Dropping new signature with empty creation time stamp; keyId="
                , thsig->keyId);
        return;
    }

    bool insert = true;
    for (auto hit = head.begin(); hit != head.end();) {
        auto hn = hit->name_string();
        auto hv = hit->value();
        if (!boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx)) {
            ++hit;
            continue;
        }

        auto hsig = HttpSignature::parse(hv);
        assert(hsig);

        if ((thsig->keyId != hsig->keyId) || (thsig->algorithm != hsig->algorithm)) {
            ++hit;
            continue;
        }

        auto hs_hdrs = sig_headers_set(hsig->headers);
        auto hs_ts = parse::number<time_t>(hsig->created);
        if (!hs_ts) {
            LOG_WARN( "Dropping existing signature with empty creation time stamp; keyId="
                    , hsig->keyId);
            hs_ts = 0;  // make it redundant
        }

        // Is inserted signature redundant?
        insert = insert && (*ths_ts > *hs_ts || has_extra_items(ths_hdrs, hs_hdrs));
        // Is existing signature redundant?
        bool keep = *hs_ts > *ths_ts || has_extra_items(hs_hdrs, ths_hdrs);

        if (keep)
            ++hit;
        else
            hit = head.erase(hit);
    }

    if (insert)
        head.insert(th.name(), thn, thv);
}

http::response_header<>
http_injection_merge( http::response_header<> rsh
                    , const http::fields& rst)
{
    rsh = SignedHead::without_framing(std::move(rsh));

    // Extend the head with trailer headers.
    for (const auto& th : rst)
        insert_trailer(th, rsh);

    return rsh;
}

template<class ArrayT>
static
boost::optional<ArrayT>
block_arrattr_from_exts(boost::string_view xs, const std::string& ext_name)
{
    if (xs.empty()) return {};  // no extensions

    sys::error_code ec;
    http::chunk_extensions xp;
    xp.parse(xs, ec);
    assert(!ec);  // this should have been validated upstream, fail hard otherwise

    auto xit = std::find_if( xp.begin(), xp.end()
                           , [&ext_name](const auto& x) {
                                 return x.first == ext_name;
                             });

    if (xit == xp.end()) return {};  // no such extension

    auto arr = util::base64_decode<ArrayT>(xit->second);

    if (!arr) LOG_WARN("Malformed chunk extension for data block: ", ext_name);

    return arr;
}

static
opt_block_digest_t
block_dig_from_exts(boost::string_view xs)
{
    return block_arrattr_from_exts<block_digest_t>(xs, http_::response_block_chain_hash_ext);
}

static
opt_sig_array_t
block_sig_from_exts(boost::string_view xs)
{
    return block_arrattr_from_exts<sig_array_t>(xs, http_::response_block_signature_ext);
}

// TODO: implement `ouipsig`

std::string
block_chunk_ext( const opt_sig_array_t& sig
               , const opt_block_digest_t& prev_digest)
{
    std::stringstream exts;

    static const auto fmt_sx = ";" + http_::response_block_signature_ext + "=\"%s\"";
    if (sig) {
        auto encoded_sig = util::base64_encode(*sig);
        exts << (boost::format(fmt_sx) % encoded_sig);
    }

    static const auto fmt_hx = ";" + http_::response_block_chain_hash_ext + "=\"%s\"";
    if (prev_digest) {
        auto encoded_hash = util::base64_encode(*prev_digest);
        exts << (boost::format(fmt_hx) % encoded_hash);
    }

    return exts.str();
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
                             ",algorithm=\"" + SignedHead::sig_alg_hs2019() + "\""
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

// begin SigningReader

using optional_part = boost::optional<http_response::Part>;

struct SigningReader::Impl {
    const http::request_header<> _rqh;
    const std::string _injection_id;
    const std::chrono::seconds::rep _injection_ts;
    const util::Ed25519PrivateKey _sk;
    const std::string _httpsig_key_id;
    ChainHasher _chain_hasher;

    Impl( http::request_header<> rqh
        , std::string injection_id
        , std::chrono::seconds::rep injection_ts
        , util::Ed25519PrivateKey sk)
        : _rqh(std::move(rqh))
        , _injection_id(std::move(injection_id))
        , _injection_ts(std::move(injection_ts))
        , _sk(std::move(sk))
        , _httpsig_key_id(SignedHead::encode_key_id(_sk.public_key()))
    {
    }

    bool _do_inject = false;
    http::response_header<> _outh;

    optional_part
    process_part(http_response::Head inh, Cancel, asio::yield_context)
    {
        auto inh_orig = inh;
        sys::error_code ec_;
        inh = util::to_cache_response(std::move(inh), ec_);
        if (ec_) return http_response::Part(std::move(inh_orig));  // will not inject, just proxy

        _do_inject = true;
        inh = cache::SignedHead( _rqh, std::move(inh)
                               , _injection_id, _injection_ts
                               , _sk);
        // We will use the trailer to send the body digest and head signature.
        assert(http::response<http::empty_body>(inh).chunked());

        _outh = inh;
        // Not useful for signing, may still be for consumer.
        inh.keep_alive(inh_orig.keep_alive());
        return http_response::Part(std::move(inh));
    }

    optional_part
    process_part(http_response::ChunkHdr, Cancel, asio::yield_context)
    {
        // Origin chunk size is ignored
        // since we use our own block size.
        // Origin chunk extensions are ignored and dropped
        // since we have no way to sign them.
        return boost::none;
    }

    size_t _body_length = 0;
    size_t _block_offset = 0;
    size_t _block_size_last = 0;
    util::SHA256 _body_hash;
    util::SHA512 _block_hash;
    // Simplest implementation: one output chunk per data block.
    util::quantized_buffer _qbuf{http_::response_data_block};
    std::queue<http_response::Part> _pending_parts;

    // If a whole data block has been processed,
    // return a chunk header and keep block as chunk body.
    optional_part
    process_part(std::vector<uint8_t> inbuf, Cancel, asio::yield_context)
    {
        // Just count transferred data and feed the hash.
        _body_length += inbuf.size();
        if (_do_inject) _body_hash.update(inbuf);
        _qbuf.put(asio::buffer(inbuf));
        auto block_buf =
            (inbuf.size() > 0) ? _qbuf.get() : _qbuf.get_rest();  // send rest if no more input

        if (block_buf.size() == 0)
            return boost::none;  // no data to send yet
        // Keep block as chunk body.
        auto block_vec = util::bytes::to_vector<uint8_t>(block_buf);
        _pending_parts.push(http_response::ChunkBody(std::move(block_vec), 0));

        http_response::ChunkHdr ch(block_buf.size(), {});

        if (_do_inject) {  // if injecting and sending data
            if (_block_offset > 0) {  // add chunk extension for previous block
                auto chain_hash = _chain_hasher.calculate_block(
                        _block_size_last, _block_hash.close(),
                        ChainHasher::Signer{_injection_id, _sk});

                ch.exts = block_chunk_ext(chain_hash.chain_signature);
            }  // else CHASH[0]=SHA2-512(DHASH[0])
            _block_hash.update(block_buf);
            _block_offset += (_block_size_last = block_buf.size());
        }

        return http_response::Part(std::move(ch));  // pass data on, drop origin extensions
    }

    http::fields _trailer_in;

    optional_part
    process_part(http_response::Trailer intr, Cancel, asio::yield_context)
    {
        _trailer_in = _do_inject ? util::to_cache_trailer(std::move(intr)) : std::move(intr);
        return boost::none;
    }

    bool _is_done = false;

    optional_part
    process_end(Cancel cancel, asio::yield_context yield)
    {
        if (_is_done) return boost::none;  // avoid adding a last chunk indefinitely

        sys::error_code ec;
        auto last_block_ch = process_part(std::vector<uint8_t>(), cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        if (last_block_ch) return last_block_ch;

        _is_done = true;

        if (!_do_inject) {
            _pending_parts.push(std::move(_trailer_in));
            return http_response::Part(http_response::ChunkHdr());
        }

        auto chain_hash = _chain_hasher.calculate_block(_block_size_last,
                _block_hash.close(), ChainHasher::Signer{_injection_id, _sk});

        auto last_ch = http_response::ChunkHdr(
                0, block_chunk_ext(chain_hash.chain_signature));

        auto trailer = cache::http_injection_trailer( _outh, std::move(_trailer_in)
                                                    , _body_length, _body_hash.close()
                                                    , _sk
                                                    , _httpsig_key_id);

        _pending_parts.push(std::move(trailer));
        return http_response::Part(std::move(last_ch));
    }
};

SigningReader::SigningReader( GenericStream in
                            , http::request_header<> rqh
                            , std::string injection_id
                            , std::chrono::seconds::rep injection_ts
                            , util::Ed25519PrivateKey sk)
    : http_response::Reader(std::move(in))
    , _impl(std::make_unique<Impl>( std::move(rqh)
                                  , std::move(injection_id)
                                  , std::move(injection_ts)
                                  , std::move(sk)))
{
}

SigningReader::~SigningReader()
{
}

optional_part
SigningReader::async_read_part(Cancel cancel, asio::yield_context yield)
{
    sys::error_code ec;
    optional_part part;

    if (!_impl->_pending_parts.empty()) {
        part = std::move(_impl->_pending_parts.front());
        _impl->_pending_parts.pop();
    }

    while (!part) {
        part = http_response::Reader::async_read_part(cancel, yield[ec]);
        assert(!_impl->_is_done || (_impl->_is_done && !part));
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        if (!part) {  // no more input, but stuff may still need to be sent
            part = _impl->process_end(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, boost::none);
            break;
        }

        part = util::apply(std::move(*part), [&](auto&& p) {
            return _impl->process_part(std::move(p), cancel, yield[ec]);
        });
        return_or_throw_on_error(yield, cancel, ec, boost::none);
    };

    return part;
}

// end SigningReader

boost::optional<HttpSignature>
HttpSignature::parse(boost::string_view sig)
{
    // TODO: proper support for quoted strings
    if (SignedHead::has_comma_in_quotes(sig)) {
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
    assert(algorithm.empty() || algorithm == SignedHead::sig_alg_hs2019());

    auto vfy_head = verification_head(rsh, *this);
    if (!vfy_head)  // e.g. because of missing headers
        return {false, {}};

    std::string sig_string;
    std::tie(sig_string, std::ignore) = get_sig_str_hdrs(*vfy_head);

    auto decoded_sig = util::base64_decode<sig_array_t>(signature);

    if (!decoded_sig) {
        LOG_WARN( "Malformed HTTP signature: ", signature);
        return {false, {}};
    }

    if (!pk.verify(sig_string, *decoded_sig))
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

// begin VerifyingReader

struct VerifyingReader::Impl {
    bool _check_framing;
    const util::Ed25519PublicKey _pk;
    const status_set _statuses;

    SignedHead _head;  // verified head; keep for later use
    boost::optional<size_t> _range_begin, _range_end;
    size_t _block_offset = 0;
    std::vector<uint8_t> _block_data;

    ChainHasher _chain_hasher;
    opt_block_digest_t _prev_block_dig;
    // Simplest implementation: one output chunk per data block.
    // If a whole data block has been processed,
    // return its chunk header and push the block as its chunk body.
    std::queue<http_response::Part> _pending_parts;

    size_t _body_length = 0;
    util::SHA256 _body_hash;

    bool _is_done = false;

    Impl(bool check_framing, util::Ed25519PublicKey pk, status_set statuses)
        : _check_framing(check_framing)
        , _pk(std::move(pk))
        , _statuses(std::move(statuses))
    {
    }

    boost::optional<http::status>
    get_original_status(const http_response::Head& inh)
    {
        if (_statuses.empty()) return boost::none;

        if (_statuses.find(inh.result()) == _statuses.end()) {
            LOG_WARN("Not replacing unaccepted HTTP status with original: ", inh.result());
            return boost::none;
        }

        auto orig_status_sv = inh[http_::response_original_http_status];
        if (orig_status_sv.empty()) return boost::none;  // no original status

        auto orig_status_uo = parse::number<unsigned>(orig_status_sv);
        if (!orig_status_uo) {
            LOG_WARN("Ignoring malformed value of original HTTP status");
            return boost::none;
        }

        auto orig_status = http::int_to_status(*orig_status_uo);
        if (orig_status == http::status::unknown) {
            LOG_WARN("Ignoring unknown value of original HTTP status: ", *orig_status_uo);
            return boost::none;
        }

        return orig_status;
    }

    optional_part
    process_part(http_response::Head inh, Cancel, asio::yield_context y)
    {
        // Restore original status if necessary.
        auto resp_status = inh.result();
        auto orig_status_o = get_original_status(inh);
        std::string resp_range;
        if (orig_status_o) {
            LOG_DEBUG( "Replacing HTTP status with original for verification: "
                     , resp_status, " -> ", *orig_status_o);
            inh.reason("");
            inh.result(*orig_status_o);
            inh.erase(http_::response_original_http_status);
            // Save `Content-Range` if `206 Partial Content`.
            if (resp_status == http::status::partial_content) {
                auto rrit = inh.find(http::field::content_range);
                if (rrit != inh.end()) {
                    resp_range = rrit->value().to_string();
                    inh.erase(rrit);
                }
            }
        }

        // Verify head signature
        auto head_o = cache::SignedHead::verify_and_create(std::move(inh), _pk);

        if (!head_o) {
            LOG_WARN("Failed to verify HTTP head signatures");
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), boost::none);
        }

        _head = std::move(*head_o);

        // Check that the response is chunked.
        if (_check_framing && !_head.chunked()) {
            LOG_WARN("Verification of non-chunked HTTP responses is not supported; uri=", _head.uri());
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), boost::none);
        }
        // Parse range in partial responses (since it may not be signed).
        if (!resp_range.empty()) {
            auto br = util::HttpResponseByteRange::parse(resp_range);
            if (!br) {
                LOG_WARN("Malformed byte range in HTTP head; uri=", _head.uri());
                return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), boost::none);
            }
            auto dszh = _head[http_::response_data_size_hdr];
            if (!br->matches_length(dszh)) {
                LOG_WARN( "Invalid byte range in HTTP head: "
                        , *br, " (/", dszh, "); uri=", _head.uri());
                return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), boost::none);
            }
            _range_begin = _block_offset = br->first;
            _range_end = br->last + 1;
        }

        _block_data.reserve(_head.block_size());

        // Return head with the status we got at the beginning.
        auto out_head = _head;
        if (orig_status_o) {
            out_head.reason("");
            out_head.result(resp_status);
            out_head.set(http_::response_original_http_status, *orig_status_o);
            // Restore `Content-Range` if `206 Partial Content`.
            if (resp_status == http::status::partial_content && !resp_range.empty())
                out_head.set(http::field::content_range, resp_range);
        }
        return http_response::Part(std::move(out_head));
    }

    optional_part
    process_part(http_response::ChunkHdr inch, Cancel cancel, asio::yield_context y)
    {
        if (inch.size > _head.block_size()) {
            LOG_WARN( "Chunk size exceeds expected data block size: "
                    , inch.size, " > ", _head.block_size(), "; uri=", _head.uri());
            return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }

        // Have we buffered a whole data block?
        // An empty data block is fine if this is the last chunk header
        // (a chunk for it will not be produced, though).

        if (_block_data.size() == 0) {
            // This is the first chunk header
            return http_response::Part{std::move(inch)};
        }

        // Verify the whole data block.
        auto block_sig = block_sig_from_exts(inch.exts);
        if (!block_sig) {
            LOG_WARN("Missing signature for data block with offset ", _block_offset, "; uri=", _head.uri());
            return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }

        // TODO: implement `ouipsig`
        // We lack the chain hash of the previous data blocks,
        // it should have been included along this block's signature.
        if (_range_begin && _block_offset > 0 && _block_offset == *_range_begin) {
            assert(!_prev_block_dig);
            _prev_block_dig = block_dig_from_exts(inch.exts);
            if (!_prev_block_dig) {
                LOG_WARN( "Missing chain hash for data block with offset "
                        , _block_offset - _head.block_size(), "; uri=", _head.uri());
                return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
            }
            _chain_hasher.set_prev_chained_digest(*_prev_block_dig);
            _chain_hasher.set_offset(_block_offset);
        }

        auto chain_hash = _chain_hasher.calculate_block(_block_data.size(), util::sha512_digest(_block_data), *block_sig);

        if (!chain_hash.verify(_head.public_key(), _head.injection_id())) {
            LOG_WARN("Failed to verify data block with offset ", _block_offset, "; uri=", _head.uri());
            return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }

        // Prepare hash for next data block: CHASH[i]=SHA2-512(CHASH[i-1] DHASH[i])
        _block_offset += _block_data.size();

        // TODO: implement `ouipsig`
        http_response::ChunkHdr ch(inch.size, block_chunk_ext(*block_sig, _prev_block_dig));
        _pending_parts.push(std::move(ch));

        _prev_block_dig = chain_hash.chain_digest;

        // Chunk header for data block (with previous extensions),
        // keep data block as chunk body.
        http_response::ChunkBody cb(std::move(_block_data), 0);
        return http_response::Part(std::move(cb));
    }

    optional_part
    process_part(std::vector<uint8_t> ind, Cancel, asio::yield_context y)
    {
        _body_length += ind.size();
        _body_hash.update(ind);

        if (_block_data.size() + ind.size() > _head.block_size()) {
            LOG_ERROR("Chunk data overflows data block boundary; uri=", _head.uri());
            return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }

        _block_data.insert(_block_data.end(), ind.begin(), ind.end());

        // Data is returned when processing chunk headers.
        return boost::none;
    }

    // If we process trailers, we may have a chance to
    // detect and signal a body not matching its signed length or digest
    // before completing its transfer,
    // so that the receiving end can see that something bad is going on.
    optional_part
    process_part(http_response::Trailer intr, Cancel, asio::yield_context y)
    {
        // Only expected trailer headers are received here, just extend initial head.
        bool sigs_in_trailer = false;
        for (const auto& h : intr) {
            auto hn = h.name_string();
            _head.insert(h.name(), hn, h.value());
            if (boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx))
                sigs_in_trailer = true;
        }
        if (sigs_in_trailer) {
            auto head_o = cache::SignedHead::verify_and_create(std::move(_head), _pk);
            if (!head_o)  // bad signature in trailer
                return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
            _head = std::move(*head_o);
        }

        return http_response::Part(std::move(intr));
    }

    void
    check_body(sys::error_code& ec)
    {
        if (_is_done) return;  // avoid re-checking body indefinitely
        _is_done = true;

        // Check body length.
        auto h_body_length_h = _head[http_::response_data_size_hdr];
        auto h_body_length = parse::number<size_t>(h_body_length_h);
        if (!h_body_length) {
            LOG_WARN("Missing signed length; uri=", _head.uri());
            ec = sys::errc::make_error_code(sys::errc::bad_message);
            return;
        }
        auto exp_body_length = ( _range_begin
                               ? *_range_end - *_range_begin
                               : *h_body_length);
        if (exp_body_length != _body_length) {
            LOG_WARN( "Body length mismatch: ", _body_length, "!=", exp_body_length
                    , "; uri=", _head.uri());
            ec = sys::errc::make_error_code(sys::errc::bad_message);
            return;
        }
        LOG_DEBUG("Body matches signed or range length: ", exp_body_length, "; uri=", _head.uri());

        // Get body digest value.
        if (_range_begin && (*_range_begin > 0 || *_range_end < *h_body_length))
            return;  // partial body, cannot check digest
        auto b_digest = http_digest(_body_hash);
        auto b_digest_s = split_string_pair(b_digest, '=');

        // Get digest values in head and compare (if algorithm matches).
        auto h_digests = _head.equal_range(http::field::digest);
        for (auto hit = h_digests.first; hit != h_digests.second; hit++) {
            auto h_digest_s = split_string_pair(hit->value(), '=');
            if (boost::algorithm::iequals(b_digest_s.first, h_digest_s.first)) {
                if (b_digest_s.second != h_digest_s.second) {
                    LOG_WARN( "Body digest mismatch: ", hit->value(), "!=", b_digest
                            , "; uri=", _head.uri());
                    ec = sys::errc::make_error_code(sys::errc::bad_message);
                    return;
                }
                LOG_DEBUG("Body matches signed digest: ", b_digest, "; uri=", _head.uri());
            }
        }
    }
};

VerifyingReader::VerifyingReader( GenericStream in
                                , util::Ed25519PublicKey pk
                                , status_set statuses)
    : http_response::Reader(std::move(in))
    , _impl(std::make_unique<Impl>(true, std::move(pk), std::move(statuses)))
{
}

VerifyingReader::VerifyingReader( GenericStream in
                                , std::unique_ptr<Impl> impl)
    : http_response::Reader(std::move(in))
    , _impl(std::move(impl))
{
}

VerifyingReader::~VerifyingReader()
{
}

optional_part
VerifyingReader::async_read_part(Cancel cancel, asio::yield_context yield)
{
    sys::error_code ec;
    optional_part part;

    if (!_impl->_pending_parts.empty()) {
        part = std::move(_impl->_pending_parts.front());
        _impl->_pending_parts.pop();
    }

    while (!part) {
        part = http_response::Reader::async_read_part(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        if (!part) break;

        part = util::apply(std::move(*part), [&](auto&& p) {
            return _impl->process_part(std::move(p), cancel, yield[ec]);
        });
        return_or_throw_on_error(yield, cancel, ec, boost::none);
    }

    if (http_response::Reader::is_done()) {
        // Check full body hash and length.
        _impl->check_body(ec);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
    }

    return part;
}

// end VerifyingReader

// begin KeepSignedReader

boost::optional<http_response::Part>
KeepSignedReader::async_read_part(Cancel cancel, asio::yield_context yield)
{
    sys::error_code ec;
    auto part = _reader.async_read_part(cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, boost::none);
    if (!part) return boost::none;  // no part
    auto headp = part->as_head();
    if (!headp) return part;  // not a head, use as is

    // Process head, remove unsigned headers.
    std::set<boost::string_view> keep_headers;
    for (const auto& hn : _extra_headers) {  // keep explicit extras
        keep_headers.emplace(hn);
    }
    for (const auto& h : *headp) {  // get set of signed headers
        auto hn = h.name_string();
        if (!boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx))
            continue;  // not a signature header
        auto hsig = HttpSignature::parse(h.value());
        assert(hsig);  // no invalid signatures should have been passed
        for (const auto& sh : SplitString(hsig->headers, ' '))
            keep_headers.emplace(sh);
    }
    for (auto hit = headp->begin(); hit != headp->end();) {  // remove unsigned (except sigs)
        auto hn = hit->name_string().to_string();
        boost::algorithm::to_lower(hn);  // signed headers are lower-case
        if ( !boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx)
           && keep_headers.find(hn) == keep_headers.end()) {
            LOG_DEBUG("Filtering out unsigned header: ", hn);
            hit = headp->erase(hit);
        } else ++hit;
    }

    return http_response::Part{*headp};
}

// end KeepSignedReader

}} // namespaces
