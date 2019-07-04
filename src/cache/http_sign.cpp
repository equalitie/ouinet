#include "http_sign.h"

#include <boost/beast/http/field.hpp>

#include "../util.h"
#include "../util/hash.h"

namespace ouinet { namespace cache {

std::string
http_digest(const http::response<http::dynamic_body>& rs)
{
    ouinet::util::SHA256 hash;

    // Feed each buffer of body data into the hash.
    for (auto it : rs.body().data())
        hash.update(it);
    auto digest = hash.close();
    auto encoded_digest = ouinet::util::base64_encode(digest);
    return "SHA-256=" + encoded_digest;
}

std::string
http_signature( const http::response_header<>& rsh
              , const ouinet::util::Ed25519PrivateKey& sk)
{
    // TODO: Compute proper signature string and sign it.
    auto fmt = boost::format("keyId=\"ed25519:%s\",algorithm=\"hs2019\"");
    // TODO: Cache somewhere, doing this every time is quite inefficient.
    auto encoded_pk = ouinet::util::base64_encode(sk.public_key().serialize());
    return (fmt % encoded_pk).str();
}

}} // namespaces
