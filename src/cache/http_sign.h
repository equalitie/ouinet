#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>

#include "../util/crypto.h"
#include "../util/hash.h"

#include "../namespaces.h"

namespace ouinet { namespace cache {

// Get an extended version of the given response head
// with added headers to support later signing the full message head.
//
// Example:
//
//     ...
//     X-Ouinet-Version: 0
//     X-Ouinet-URI: https://example.com/foo
//     X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
//     X-Ouinet-HTTP-Status: 200
//     Transfer-Encoding: chunked
//     Trailer: X-Ouinet-Data-Size, Digest, Signature
//
http::response_header<>  // use this to enable setting the time stamp (e.g. for tests)
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id
                   , std::chrono::seconds::rep injection_ts);

inline
http::response_header<>  // use this for the rest of cases
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_injection_head(rqh, std::move(rsh), injection_id, ts);
}

// Get an extended version of the given response trailer
// with added headers completing the signature of the message.
//
// Example:
//
//     ...
//     X-Ouinet-Data-Size: 38
//     Digest: SHA-256=j7uwtB/QQz0FJONbkyEmaqlJwGehJLqWoCO1ceuM30w=
//     Signature: keyId="...",algorithm="hs2019",created=1516048311,
//       headers="(created) ... digest",signature="..."
//
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey&
                      , const std::string key_id
                      , std::chrono::seconds::rep ts);

inline
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey& sk
                      , const std::string key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_injection_trailer( rsh, std::move(rst)
                                 , content_length, content_digest
                                 , sk, key_id, ts);
}

// Get a `keyId` encoding the given public key itself.
std::string
http_key_id_for_injection(const ouinet::util::Ed25519PublicKey&);

// Get the body digest as per RFC 3230 and RFC 5843.
//
// Example:
//
//     SHA-256=NYfLd2zg5OgjfyFYALff+6DyWGXLhFUOh+qLusg4xCM=
//
std::string
http_digest(const http::response<http::dynamic_body>&);

// Compute a signature as per draft-cavage-http-signatures-11.
std::string  // use this to enable setting the time stamp (e.g. for tests)
http_signature( const http::response_header<>&
              , const ouinet::util::Ed25519PrivateKey&
              , const std::string key_id
              , std::chrono::seconds::rep ts);

inline  // use this for the rest of cases
std::string
http_signature( const http::response_header<>& rsh
              , const ouinet::util::Ed25519PrivateKey& sk
              , const std::string key_id)
{
    auto ts = std::chrono::seconds(std::time(nullptr)).count();
    return http_signature(rsh, sk, key_id, ts);
}

}} // namespaces
