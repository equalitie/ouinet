#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/regex.hpp>

#include "../constants.h"
#include "../response_reader.h"
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

// Merge the response head `rsh` and response trailer `rst` into a single head,
// removing signatures redundant with those in the trailer.
//
// Signature B is considered redundant regarding signature A
// if A has the same `keyId` and `algorithm`,
// the same or a newer `created` time stamp,
// and the same or a larger set of `headers`.
// If all these values are equal, the later signature in the head or trailer
// is considered redundant.
//
// Please note that framing headers are also removed,
// so if you want to reuse the header in a response,
// you must either add a `Content-Length` or a `Transfer-Encoding: chunked` header.
http::response_header<>
http_injection_merge( http::response_header<> rsh
                    , const http::fields& rst);

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

    bool
    is_done() const override;

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

    bool
    is_done() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};


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
