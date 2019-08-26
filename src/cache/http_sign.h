#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>

#include "../constants.h"
#include "../session.h"
#include "../util/crypto.h"
#include "../util/hash.h"

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
}}

namespace ouinet { namespace cache {

// Ouinet-specific declarations for injection using HTTP signatures
// ----------------------------------------------------------------

namespace http_sign_detail {
bool check_body(const http::response_header<>&, size_t, ouinet::util::SHA256&);
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
//     X-Ouinet-Sig0: keyId="...",algorithm="hs2019",created=1516048310,
//       headers="(response-status) (created) ... x-ouinet-injection",
//       signature="..."
//     Transfer-Encoding: chunked
//     Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1
//
http::response_header<>  // use this to enable setting the time stamp (e.g. for tests)
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , std::chrono::seconds::rep injection_ts
                   , const ouinet::util::Ed25519PrivateKey&
                   , const std::string& key_id);

inline
http::response_header<>  // use this for the rest of cases
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , const ouinet::util::Ed25519PrivateKey& sk
                   , const std::string& key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_injection_head(rqh, std::move(rsh), injection_id, ts, sk, key_id);
}

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
    size_t body_length = 0;
    util::SHA256 body_hash;
    // If we process trailers, we may have a chance to
    // detect and signal a body not matching its signed length or digest
    // before completing its transfer,
    // so that the receiving end can see that something bad is going on.
    bool check_body_after = true;

    auto hproc = [&] (auto inh, auto&, auto y) {
        inh = cache::http_injection_verify(move(inh), pk);
        if (inh.cbegin() == inh.cend())
            return or_throw(y, sys::errc::make_error_code(sys::errc::no_message), inh);
        head = inh;
        return inh;
    };

    ProcInFunc<asio::const_buffer> dproc = [&] (auto ind, auto&, auto) {
        body_length += ind.size();
        body_hash.update(ind);
        return ind;
    };

    auto tproc = [&] (auto intr, auto&, auto y) {
        if (intr.cbegin() == intr.cend())
            return intr;  // no headers in trailer

        // Only expected trailer headers are received here, just extend initial head.
        bool sigs_in_trailer = false;
        for (const auto& h : intr) {
            auto hn = h.name_string();
            head.insert(h.name(), hn, h.value());
            if (boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx))
                sigs_in_trailer = true;
        }
        if (sigs_in_trailer) {
            head = cache::http_injection_verify(std::move(head), pk);
            if (head.cbegin() == head.cend())  // bad signature in trailer
                return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), intr);
        }

        check_body_after = false;
        if (!http_sign_detail::check_body(head, body_length, body_hash))
            return or_throw(y, sys::errc::make_error_code(sys::errc::bad_message), intr);

        return intr;
    };

    sys::error_code ec;
    in.flush_response( out
                     , std::move(hproc), std::move(dproc), std::move(tproc)
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
