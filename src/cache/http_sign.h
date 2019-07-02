#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/format.hpp>

#include "../constants.h"

#include "../namespaces.h"

namespace ouinet { namespace cache {

// Add a body digest as per RFC 3230 and RFC 5843.
//
// Example:
//
//     Digest: SHA-256=NYfLd2zg5OgjfyFYALff+6DyWGXLhFUOh+qLusg4xCM=
//
http::response<http::dynamic_body>
http_add_digest(http::response<http::dynamic_body>);

// Add internal headers to support signing the message head.
//
// Example:
//
//     X-Ouinet-Version: 0
//     X-Ouinet-URI: https://example.com/foo
//     X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
//     X-Ouinet-HTTP-Status: 200
//
template<class Request, class Response>
inline
Response
http_add_injection_meta( const Request& canon_rq, Response rs
                       , const std::string& injection_id)
{
    using namespace ouinet::http_;
    assert(response_version_hdr_current == response_version_hdr_v0);

    rs.set(response_version_hdr, response_version_hdr_v0);
    rs.set(header_prefix + "URI", canon_rq.target());
    {
        auto ts = std::chrono::seconds(std::time(nullptr)).count();
        rs.set( header_prefix + "Injection"
              , boost::format("id=%s,ts=%d") % injection_id % ts);
    }
    rs.set(header_prefix + "HTTP-Status", rs.result_int());

    return rs;
}

namespace http_sign_detail {
std::string get_signature(const http::response_header<>&);
}

// TODO: Use a key and its signature algorithm to
// actually create a signature
// according to draft-cavage-http-signatures-11.
template<class Response>
inline
Response
http_add_signature(Response rs)
{
    auto sig = http_sign_detail::get_signature(rs);
    rs.set("Signature", sig);
    return rs;
}

}} // namespaces
