#pragma once

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
http::response_header<>
http_injection_head( const http::request_header<>& rqh
                   , http::response_header<> rsh
                   , const std::string& injection_id);

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
http::fields
http_injection_trailer( const http::response_header<>& rsh
                      , http::fields rst
                      , size_t content_length
                      , const ouinet::util::SHA256::digest_type& content_digest
                      , const ouinet::util::Ed25519PrivateKey&);

// Get the body digest as per RFC 3230 and RFC 5843.
//
// Example:
//
//     SHA-256=NYfLd2zg5OgjfyFYALff+6DyWGXLhFUOh+qLusg4xCM=
//
std::string
http_digest(const http::response<http::dynamic_body>&);

// Compute a signature as per draft-cavage-http-signatures-11.
std::string
http_signature( const http::response_header<>&
              , const ouinet::util::Ed25519PrivateKey&);

}} // namespaces
