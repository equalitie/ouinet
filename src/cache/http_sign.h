#pragma once

#include <chrono>
#include <ctime>
#include <set>
#include <string>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/regex.hpp>

#include "../constants.h"
#include "../response_reader.h"
#include "../util/crypto.h"
#include "../util/hash.h"
#include "../util/executor.h"

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

    // This contains the originally signed HTTP status code
    // if a signed response was transformed in
    // a partial response or a head response.
    // If present, this header replaces the actual response status
    // for verification purposes.
    static const std::string response_original_http_status = header_prefix + "HTTP-Status";

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

using ouinet::util::AsioExecutor;

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

// Get a `keyId` encoding the given public key itself.
std::string
http_key_id_for_injection(const ouinet::util::Ed25519PublicKey&);

// Create HTTP chunk extension
std::string
block_chunk_ext( const boost::optional<util::Ed25519PublicKey::sig_array_t>& sig
               , const boost::optional<util::SHA512::digest_type>& prev_digest = {});

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
// By default,
// responses with a signed `(response-status)` are only considered valid
// when they have the same HTTP status used for creating their signatures.
// If a set of HTTP `statuses` is provided,
// responses derived from the originally signed response
// but having one of the given statuses are accepted too,
// as long as the original status code appears as `X-Ouinet-HTTP-Status`.
// This can be used to verify partial or "not modified" responses
// based on a signed full response to a `GET` request.
//
// The read operation fails with error `boost::system::errc::no_message`
// if the response head failed to be verified or was not acceptable;
// or with error `boost::system::errc::bad_message`
// if verification fails later on.
//
// The resulting output preserves all the information and formatting needed
// to be verified again.
class VerifyingReader : public ouinet::http_response::AbstractReader {
public:
    using reader_uptr = std::unique_ptr<ouinet::http_response::AbstractReader>;
    using status_set = std::set<http::status>;

public:
    VerifyingReader( GenericStream in, ouinet::util::Ed25519PublicKey pk
                   , status_set statuses = {});
    VerifyingReader( reader_uptr rd, ouinet::util::Ed25519PublicKey pk
                   , status_set statuses = {});
    ~VerifyingReader() override;

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel, asio::yield_context) override;

    bool is_done() const override { return _reader->is_done(); }
    void close() override { _reader->close(); }
    AsioExecutor get_executor() override { return _reader->get_executor(); }

private:
    struct Impl;
    reader_uptr _reader;
    std::unique_ptr<Impl> _impl;
};

// Filters out headers not included in the set of signed headers
// (with the exception of signatures themselves).
// Headers in the `extra` set are also kept.
//
// The input is assumed to already have correct signatures,
// they are not verified again.
//
// Use this reader to clean a signed response from
// headers added after its verification
// (e.g. used for internal purposes).
class KeepSignedReader : public ouinet::http_response::AbstractReader {
public:
    KeepSignedReader( ouinet::http_response::AbstractReader& r
                    , std::set<std::string> extra = {})
        : _reader(r)
    {
        for (const auto& hn : extra)  // store lower-case copies
            _extra_headers.emplace(boost::algorithm::to_lower_copy(hn));
    }

    ~KeepSignedReader() override {}

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel, asio::yield_context) override;

    bool is_done() const override { return _reader.is_done(); }
    void close() override { _reader.close(); }

    AsioExecutor get_executor() override
    {
        return _reader.get_executor();
    }

private:
    ouinet::http_response::AbstractReader& _reader;
    std::set<std::string> _extra_headers;
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
