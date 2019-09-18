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

    // A default size for data blocks to be signed.
    // Small enough to avoid nodes buffering too much data
    // and not take too much time to download on slow connections,
    // but big enough to completely cover most responses
    // and thus avoid having too many signatures per response.
    static const size_t response_data_block = 65536;  // TODO: sensible value
}}

namespace ouinet { namespace cache {

// Ouinet-specific declarations for injection using HTTP signatures
// ----------------------------------------------------------------

namespace http_sign_detail {
util::SHA256 block_base_hash(const std::string&, size_t);
std::string block_chunk_ext(util::SHA256&, const util::Ed25519PrivateKey&);
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
//     X-Ouinet-Version: 0
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

// Flush a response from session `in` to stream `out`
// while signing with the provided private key.
template<class SinkStream>
inline
void
session_flush_signed( Session& in, SinkStream& out
                    , const http::request_header<>& rqh
                    , const std::string& injection_id
                    , std::chrono::seconds::rep injection_ts
                    , const ouinet::util::Ed25519PrivateKey& sk
                    , Cancel& cancel, asio::yield_context yield)
{
    auto httpsig_key_id = http_key_id_for_injection(sk.public_key());  // TODO: cache this

    bool do_inject = false;
    http::response_header<> outh;
    auto hproc = [&] (auto inh, auto&, auto) {
        auto inh_orig = inh;
        sys::error_code ec_;
        inh = util::to_cache_response(move(inh), ec_);
        if (ec_) return inh_orig;  // will not inject, just proxy

        do_inject = true;
        inh = cache::http_injection_head( rqh, move(inh)
                                        , injection_id, injection_ts
                                        , sk, httpsig_key_id);
        // We will use the trailer to send the body digest and head signature.
        assert(http::response<http::empty_body>(inh).chunked());

        outh = inh;
        return inh;
    };

    auto xproc = [] (auto, auto&, auto) {
        // Origin chunk extensions are ignored and dropped
        // since we have no way to sign them.
    };

    size_t body_length = 0;
    size_t block_offset = 0;
    util::SHA256 body_hash;
    util::SHA256 block_hash  // for first block
        = http_sign_detail::block_base_hash(injection_id, block_offset);
    // Simplest implementation: one output chunk per data block.
    // The big buffer may cause issues with coroutine stack management,
    // so allocate it in the heap.
    auto qbuf = std::make_unique<util::quantized_buffer<http_::response_data_block>>();
    ProcDataFunc<asio::const_buffer> dproc = [&] (auto inbuf, auto&, auto) {
        // Just count transferred data and feed the hash.
        body_length += inbuf.size();
        if (do_inject) body_hash.update(inbuf);
        qbuf->put(inbuf);
        ProcDataFunc<asio::const_buffer>::result_type ret{
            (inbuf.size() > 0) ? qbuf->get() : qbuf->get_rest(), {}
        };  // send rest if no more input
        if (do_inject && ret.first.size() > 0) {  // if injecting and sending data
            if (block_offset > 0)  // add chunk extension for previous block
                ret.second = http_sign_detail::block_chunk_ext(block_hash, sk);
            // Prepare chunk extension for next block.
            block_hash = http_sign_detail::block_base_hash(injection_id, block_offset);
            block_hash.update(ret.first);
            block_offset += ret.first.size();
        }
        return ret;  // pass data on, drop origin extensions
    };

    ProcTrailFunc tproc = [&] (auto intr, auto&, auto) {
        if (do_inject) {
            intr = util::to_cache_trailer(move(intr));
            intr = cache::http_injection_trailer( outh, move(intr)
                                                , body_length, body_hash.close()
                                                , sk
                                                , httpsig_key_id);
        }
        ProcTrailFunc::result_type ret{move(intr), {}};
        if (do_inject)
            ret.second = http_sign_detail::block_chunk_ext(block_hash, sk);
        return ret;  // pass trailer on, drop origin extensions
    };

    sys::error_code ec;
    in.flush_response( out
                     , std::move(hproc), std::move(xproc), std::move(dproc), std::move(tproc)
                     , cancel, yield[ec]);
    return or_throw(yield, ec);
}

// Flush a response from session `in` to stream `out`
// while verifying signatures by the provided public key.
//
// Fail with error `boost::system::errc::no_message`
// if the response head failed to be verified
// (in which case no data should have been sent to `out`);
// fail with error `boost::system::errc::bad_message`
// if verification fails later on
// (in which case data may have already been sent).
template<class SinkStream>
inline
void
session_flush_verified( Session& in, SinkStream& out
                      , const ouinet::util::Ed25519PublicKey& pk
                      , Cancel& cancel, asio::yield_context yield)
{
    http::response_header<> head;
    auto hproc = [&] (auto inh, auto&, auto y) {
        inh = cache::http_injection_verify(move(inh), pk);
        if (inh.cbegin() == inh.cend())
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), inh);
        head = inh;
        return inh;
    };

    auto xproc = [] (auto, auto&, auto) {
        // Chunk extensions are not forwarded
        // since we have no way to verify them.
    };

    size_t body_length = 0;
    util::SHA256 body_hash;
    ProcDataFunc<asio::const_buffer> dproc = [&] (auto ind, auto&, auto) {
        body_length += ind.size();
        body_hash.update(ind);
        ProcDataFunc<asio::const_buffer>::result_type ret{std::move(ind), {}};
        return ret;  // pass data on, drop chunk extensions
    };

    // If we process trailers, we may have a chance to
    // detect and signal a body not matching its signed length or digest
    // before completing its transfer,
    // so that the receiving end can see that something bad is going on.
    bool check_body_after = true;
    ProcTrailFunc tproc = [&] (auto intr, auto&, auto y) {
        ProcTrailFunc::result_type ret{std::move(intr), {}};  // pass trailer on, drop chunk extensions

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
// <https://tools.ietf.org/html/draft-cavage-http-signatures-11>.

// Compute a signature as per draft-cavage-http-signatures-11.
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

private:
    static
    bool has_comma_in_quotes(const boost::string_view&);
};

}} // namespaces
