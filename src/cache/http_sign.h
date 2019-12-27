#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>

#include "../constants.h"
#include "../http_util.h"
#include "../http_forward.h"
#include "../logger.h"
#include "../response_reader.h"
#include "../session.h"
#include "../util/crypto.h"
#include "../util/hash.h"
#include "../util/quantized_buffer.h"

#include "../namespaces.h"

namespace ouinet { namespace http_ {
    // A prefix for HTTP signature headers at the response head,
    // each of them followed by a non-repeating, 0-based decimal integer.
    static const std::string response_signature_hdr_pfx = header_prefix + "Sig";
    // The corresponding regular expression, capturing the number.
    static const boost::regex response_signature_hdr_rx( response_signature_hdr_pfx + "([0-9]+)"
                                                       , boost::regex::normal|boost::regex::icase);
    // This allows signing the size of body data
    // without breaking on transfer encoding changes.
    static const std::string response_data_size_hdr = header_prefix + "Data-Size";

    // This contains common parameters for block signatures.
    static const std::string response_block_signatures_hdr = header_prefix + "BSigs";

    // Chunk extension used to hold data block signature.
    static const std::string response_block_signature_ext = "ouisig";

    // Chunk extension used to hold data block chained hashes.
    static const std::string response_block_chain_hash_ext = "ouihash";

    // A default size for data blocks to be signed.
    // Small enough to avoid nodes buffering too much data
    // and not take too much time to download on slow connections,
    // but big enough to completely cover most responses
    // and thus avoid having too many signatures per response.
    static const size_t response_data_block = 65536;  // TODO: sensible value

    // Maximum data block size that a receiver is going to accept.
    static const size_t response_data_block_max = 1024 * 1024;  // TODO: sensible value
}}

namespace ouinet { namespace cache {

// Ouinet-specific declarations for injection using HTTP signatures
// ----------------------------------------------------------------

namespace http_sign_detail {
using sig_array_t = util::Ed25519PublicKey::sig_array_t;
using block_digest_t = util::SHA512::digest_type;
using opt_sig_array_t = boost::optional<sig_array_t>;
using opt_block_digest_t = boost::optional<block_digest_t>;

opt_sig_array_t block_sig_from_exts(boost::string_view);
std::string block_sig_str(boost::string_view, const block_digest_t&);
std::string block_chunk_ext( boost::string_view, const block_digest_t&
                           , const util::Ed25519PrivateKey&);
std::string block_chunk_ext(const opt_sig_array_t&, const opt_block_digest_t&);
bool check_body(const http::response_header<>&, size_t, util::SHA256&);
}

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
http::response_header<>
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , std::chrono::seconds::rep injection_ts
                   , const ouinet::util::Ed25519PrivateKey&
                   , const std::string& key_id);

// Get an extended version of the given response trailer
// with added headers completing the signature of the message.
//
// Please note that framing headers (`Content-Length`, `Transfer-Encoding`, `Trailer`)
// are not included in the signature, though an `X-Ouinet-Data-Size` header is added to
// convey the actual content length after the whole content has been seen.
// If a non-chunked response head needs to be constructed from the signed head,
// a `Content-Length` header should be added with the value of `X-Ouinet-Data-Size`
// (and the later be kept as well to avoid a signature verification failure).
//
// The signature of the initial head (`X-Ouinet-Sig0`) is not included among
// the signed headers, so that the receiver may replace it with
// the value of the signature in the trailer (`X-Ouinet-Sig1`)
// for subsequent uses.
//
// Example:
//
//     ...
//     X-Ouinet-Data-Size: 38
//     Digest: SHA-256=j7uwtB/QQz0FJONbkyEmaqlJwGehJLqWoCO1ceuM30w=
//     X-Ouinet-Sig1: keyId="...",algorithm="hs2019",created=1516048311,
//       headers="(response-status) (created) ... x-ouinet-injection x-ouinet-data-size digest",
//       signature="..."
//
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey&
                      , const std::string& key_id
                      , std::chrono::seconds::rep ts);

inline
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey& sk
                      , const std::string& key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_injection_trailer( rsh, std::move(rst)
                                 , content_length, content_digest
                                 , sk, key_id, ts);
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
http::response_header<>
http_injection_verify( http::response_header<>
                     , const ouinet::util::Ed25519PublicKey&);

// Get a `keyId` encoding the given public key itself.
std::string
http_key_id_for_injection(const ouinet::util::Ed25519PublicKey&);

// Decode the given `keyId` into a public key.
boost::optional<util::Ed25519PublicKey>
http_decode_key_id(boost::string_view key_id);

// A simple container for a parsed block signatures HTTP header.
// Only the `hs2019` algorithm with an explicit key is supported,
// so the ready-to-use key is left in `pk`.
struct HttpBlockSigs {
    util::Ed25519PublicKey pk;
    boost::string_view algorithm;  // always "hs2019"
    size_t size;

    static
    boost::optional<HttpBlockSigs> parse(boost::string_view);
};

// Allows reading parts of a response from stream `in`
// while signing with the private key `sk`.
class SigningReader : public ouinet::http_response::Reader {
public:
    SigningReader( GenericStream in
                 , http::request_header<> rqh
                 , std::string injection_id
                 , std::chrono::seconds::rep injection_ts
                 , ouinet::util::Ed25519PrivateKey sk);
    ~SigningReader() override;

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel, asio::yield_context) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Allows reading parts of a response from stream `in`
// while verifying signatures from the public key `pk`.
//
// The read operation fails with error `boost::system::errc::no_message`
// if the response head failed to be verified or was not acceptable;
// or with error `boost::system::errc::bad_message`
// if verification fails later on.
//
// The resulting output preserves all the information and formatting needed
// to be verified again.
class VerifyingReader : public ouinet::http_response::Reader {
public:
    VerifyingReader(GenericStream in, ouinet::util::Ed25519PublicKey pk);
    ~VerifyingReader() override;

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel, asio::yield_context) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Flush a response from session `in` to stream `out`
// while verifying signatures by the provided public key.
//
// Fail with error `boost::system::errc::no_message`
// if the response head failed to be verified or was not acceptable
// (in which case no data should have been sent to `out`);
// fail with error `boost::system::errc::bad_message`
// if verification fails later on
// (in which case data may have already been sent).
//
// The resulting output contains all the information and formatting needed
// to be verified again.
template<class SinkStream>
inline
void
session_flush_verified( Session& in, SinkStream& out
                      , const ouinet::util::Ed25519PublicKey& pk
                      , Cancel& cancel, asio::yield_context yield)
{
#ifndef NDEBUG // debug
#   warning TODO
    sys::error_code ec;
    in.flush_response(out, cancel, yield[ec]);
    return or_throw(yield, ec);
#else // release
    http::response_header<> head;  // keep for refs and later use
    boost::string_view uri;  // for warnings, should use `Yield::log` instead
    boost::string_view injection_id;
    boost::optional<HttpBlockSigs> bs_params;
    std::unique_ptr<util::quantized_buffer> qbuf;
    auto hproc = [&] (auto inh, auto&, auto y) {
        // Verify head signature.
        head = cache::http_injection_verify(move(inh), pk);
        if (head.cbegin() == head.cend()) {
            LOG_WARN("Failed to verify HTTP head signatures");
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), head);
        }
        uri = head[http_::response_uri_hdr];
        // Get and validate HTTP block signature parameters.
        auto bsh = head[http_::response_block_signatures_hdr];
        if (bsh.empty()) {
            LOG_WARN("Missing parameters for HTTP data block signatures; uri=", uri);
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), head);
        }
        bs_params = cache::HttpBlockSigs::parse(bsh);
        if (!bs_params) {
            LOG_WARN("Malformed parameters for HTTP data block signatures; uri=", uri);
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), head);
        }
        if (bs_params->size > http_::response_data_block_max) {
            LOG_WARN("Size of signed HTTP data blocks is too large: ", bs_params->size, "; uri=", uri);
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), head);
        }
        // The injection id is also needed to verify block signatures.
        injection_id = util::http_injection_id(head);
        if (injection_id.empty()) {
            LOG_WARN("Missing injection identifier in HTTP head; uri=", uri);
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), head);
        }
        qbuf = std::make_unique<util::quantized_buffer>(bs_params->size);
        return head;
    };

    http_sign_detail::opt_sig_array_t inbsig, outbsig;
    auto xproc = [&inbsig, &uri] (auto inx_, auto&, auto) {
        auto inbsig_ = http_sign_detail::block_sig_from_exts(inx_);
        if (!inbsig_) return;

        // Capture and keep the latest block signature only.
        if (inbsig)
            LOG_WARN("Dropping data block signature; uri=", uri);
        inbsig = std::move(inbsig_);
    };

    size_t body_length = 0;
    size_t block_offset = 0;
    util::SHA256 body_hash;
    util::SHA512 block_hash;
    std::vector<char> lastd_;
    asio::mutable_buffer lastd;
    http_sign_detail::opt_block_digest_t inbdig, outbdig;
    // Simplest implementation: one output chunk per data block.
    ProcDataFunc<asio::const_buffer> dproc = [&] (auto ind, auto&, auto y) {
        // Data block is sent when data following it is received
        // (so process the last data buffer now).
        body_length += lastd.size();
        body_hash.update(lastd);
        qbuf->put(lastd);
        ProcDataFunc<asio::const_buffer>::result_type ret{
            (ind.size() > 0) ? qbuf->get() : qbuf->get_rest(), {}
        };  // send rest if no more input
        if (ret.first.size() > 0) {  // send last extensions, keep current for next
            // Verify signature of data block to be sent (fail if missing).
            if (!inbsig) {
                LOG_WARN("Missing signature for data block with offset ", block_offset, "; uri=", uri);
                return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), ret);
            }
            // Complete hash for this block; note that HASH[0]=SHA2-512(BLOCK[0])
            block_hash.update(ret.first);
            auto block_digest = block_hash.close();
            auto bsig_str = http_sign_detail::block_sig_str(injection_id, block_digest);
            if (!bs_params->pk.verify(bsig_str, *inbsig)) {
                LOG_WARN("Failed to verify data block with offset ", block_offset, "; uri=", uri);
                return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), ret);
            }

            ret.second = http_sign_detail::block_chunk_ext(outbsig, outbdig);
            outbsig = std::move(inbsig);
            inbsig = {};
            // Prepare hash for next block: HASH[i]=SHA2-512(HASH[i-1] BLOCK[i])
            block_hash = {}; block_hash.update(block_digest);
            block_offset += ret.first.size();
            // Chain hash is to be sent along the signature of the following block,
            // so that it may convey the missing information for computing the signing string
            // if the receiver does not have the previous blocks (e.g. for range requests).
            // (Bk0) (Sig0 Bk1) (Sig1 Hash0 Bk2) ... (SigN-1 HashN-2 BkN) (SigN HashN-1)
            outbdig = std::move(inbdig);
            inbdig = std::move(block_digest);
        }

        // Save copy of current input data to last data buffer.
        if (ind.size() > lastd_.size())  // extend storage if needed
            lastd_.resize(ind.size());
        lastd = asio::buffer(lastd_.data(), ind.size());
        asio::buffer_copy(lastd, ind);

        return ret;  // pass data on
    };

    // If we process trailers, we may have a chance to
    // detect and signal a body not matching its signed length or digest
    // before completing its transfer,
    // so that the receiving end can see that something bad is going on.
    bool check_body_after = true;
    ProcTrailFunc tproc = [&] (auto intr, auto&, auto y) {
        ProcTrailFunc::result_type ret{
            std::move(intr), http_sign_detail::block_chunk_ext(outbsig, outbdig)};  // pass trailer on

        if (ret.first.cbegin() == ret.first.cend())
            return ret;  // no headers in trailer

        // Only expected trailer headers are received here, just extend initial head.
        bool sigs_in_trailer = false;
        for (const auto& h : ret.first) {
            auto hn = h.name_string();
            head.insert(h.name(), hn, h.value());
            if (boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx))
                sigs_in_trailer = true;
        }
        if (sigs_in_trailer) {
            head = cache::http_injection_verify(std::move(head), pk);
            if (head.cbegin() == head.cend())  // bad signature in trailer
                return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), ret);
        }

        check_body_after = false;
        if (!http_sign_detail::check_body(head, body_length, body_hash))
            return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), ret);

        return ret;
    };

    sys::error_code ec;
    in.flush_response( out
                     , std::move(hproc), std::move(xproc), std::move(dproc), std::move(tproc)
                     , cancel, yield[ec]);
    if (!ec && check_body_after)
        if (!http_sign_detail::check_body(head, body_length, body_hash))
            ec = sys::errc::make_error_code(sys::errc::bad_message);
    return or_throw(yield, ec);
#endif
}


// Body digest computation as per RFC 3230 and RFC 5843
// ----------------------------------------------------
//
// Example:
//
//     SHA-256=NYfLd2zg5OgjfyFYALff+6DyWGXLhFUOh+qLusg4xCM=
//
std::string
http_digest(ouinet::util::SHA256&);

std::string
http_digest(const http::response<http::dynamic_body>&);


// Generic HTTP signatures
// -----------------------
//
// These provide access to an implementation of
// <https://tools.ietf.org/html/draft-cavage-http-signatures-12>.

// Compute a signature as per draft-cavage-http-signatures-12.
std::string  // use this to enable setting the time stamp (e.g. for tests)
http_signature( const http::response_header<>&
              , const ouinet::util::Ed25519PrivateKey&
              , const std::string& key_id
              , std::chrono::seconds::rep ts);

inline  // use this for the rest of cases
std::string
http_signature( const http::response_header<>& rsh
              , const ouinet::util::Ed25519PrivateKey& sk
              , const std::string& key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_signature(rsh, sk, key_id, ts);
}

// A simple container for a parsed HTTP signature,
// e.g. as produced by `http_signature`.
// Use the `parse` static method to parse the signature string into its components,
// then use `verify` to check the signature against a public key,
// which should be the same as that specified by the signature's `keyId`,
// though how they are both linked is out of the scope of this code.
//
// Please note that all members point to the original signature string,
// so it should be alive while using this.
struct HttpSignature {
    boost::string_view keyId;
    boost::string_view algorithm;
    boost::string_view created;
    boost::string_view expires;
    boost::string_view headers;
    boost::string_view signature;

    static
    boost::optional<HttpSignature> parse(boost::string_view);

    // Return whether the given head does match the signature
    // for the headers covered by the latter.
    // If so, also indicate which other extra headers are
    // present in the head but not covered by the signature
    // (extra field names and values point to the given head).
    std::pair<bool, http::fields> verify( const http::response_header<>&
                                        , const util::Ed25519PublicKey&);
};

}} // namespaces
