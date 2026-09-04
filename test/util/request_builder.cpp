#include "request_builder.h"

#include "util/str.h"
#include "constants.h"

#include <boost/beast/version.hpp>

namespace ouinet {

http::request<http::string_body> build_origin_request(const util::Url& url) {
    int version = 11;

    std::string host = url.host;
    if (!url.port.empty()) host += ":" + url.port;
    std::string target = url.path;

    http::request<http::string_body> req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    return req;
}

http::request<http::string_body> build_private_request(const util::Url& url) {
    int version = 11;

    std::string host = url.host;
    if (!url.port.empty()) host += ":" + url.port;
    std::string target = url.reassemble();

    http::request<http::string_body> req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_private_hdr, "true");
    req.prepare_payload();
    return req;
}

http::request<http::string_body> CacheRequestBuilder::build() const {
    int version = 11;

    std::string host = url.host;
    if (!url.port.empty()) host += ":" + url.port;
    std::string target = url.reassemble();

    http::request<http::string_body> req{http::verb::get, target, version};

    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    if (resource_group) {
        req.set(http_::request_group_hdr, *resource_group);
    }
    else {
        req.set(http_::request_group_hdr, target);
    }

    if (route) {
        req.set("X-Ouinet-Route", util::str(*route));
    }

    return req;
}

} // namespace
