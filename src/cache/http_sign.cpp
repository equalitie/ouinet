#include "http_sign.h"

#include <boost/beast/http/field.hpp>

#include "../util.h"
#include "../util/hash.h"

namespace ouinet { namespace cache {

http::response<http::dynamic_body>
http_add_digest(http::response<http::dynamic_body> rs)
{
    ouinet::util::SHA256 hash;

    // Feed each buffer of body data into the hash.
    for (auto it : rs.body().data())
        hash.update(it);
    auto digest = hash.close();
    auto encoded_digest = ouinet::util::base64_encode(digest);
    rs.set(http::field::digest, "SHA-256=" + encoded_digest);

    return rs;
}

std::string
http_sign_detail::get_signature(const http::response_header<>& rsh)
{
    // TODO
    return "";
}

}} // namespaces
