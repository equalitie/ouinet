#pragma once

#include <boost/beast/core/detail/base64.hpp>
#include "namespaces.h"
#include "generic_stream.h"
#include "http_util.h"
#include "util.h"

namespace ouinet {

namespace authenticate_detail {
    inline std::string parse_auth(beast::string_view encoded)
    {
        while(encoded.starts_with(" ")) encoded.remove_prefix(1);
        while(encoded.ends_with(" "))   encoded.remove_suffix(1);

        if (encoded.starts_with("Basic")) {
            encoded.remove_prefix(strlen("Basic"));
        } else {
            return {};
        }

        while(encoded.starts_with(" ")) encoded.remove_prefix(1);

        std::string decoded = ouinet::util::base64_decode(encoded);

        // Trim the Unicode character U+00A3 (POUND SIGN) from the end if present.
        if (const auto s = decoded.size() >= 2) {
            if (decoded[s - 1] == 0xa3 && decoded[s - 2] == 0xc2) {
                decoded.resize(s - 2);
            }
        }

        return decoded;
    }
}

// https://developer.mozilla.org/en-US/docs/Web/HTTP/Authentication
//
// This times out if an authentication error message fails to be sent.
template<class Request>
inline
bool authenticate( Request& req
                 , GenericStream& con
                 , beast::string_view credentials /* e.g.: "test:123" */
                 , asio::yield_context yield)
{
    using namespace authenticate_detail;

    if (credentials.empty()) return true;

    auto auth_i = req.find(http::field::proxy_authorization);

    if (auth_i != req.end()) {
        bool valid = credentials == parse_auth(auth_i->value());

        // Make sure we don't pass the credentials further.
        req.erase(http::field::proxy_authorization);

        if (valid) return true;
    }

    http::response<http::string_body>
        res{http::status::proxy_authentication_required,
            req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set( http::field::proxy_authenticate
           , "Basic realm=\"Ouinet injector\"");

    res.prepare_payload();

    sys::error_code ec;
    util::http_reply(con, res, yield[ec]);

    return or_throw(yield, ec, false);
}

template<class Request>
inline
Request authorize( const Request& req
                 , beast::string_view credentials /* e.g.: "test:123" */)
{
    std::string c = ouinet::util::base64_encode(credentials);

    Request ret = req;

    ret.set(http::field::proxy_authorization, "Basic " + c);
    ret.prepare_payload();

    return ret;
}

} // ouinet namespace
