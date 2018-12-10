#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast.hpp>
#include "../namespaces.h"
#include "../util.h"

namespace ouinet {

template <class Request>
inline
std::string key_from_http_req(const Request& req) {
    // The key is currently the canonical URL itself.
    return util::canonical_url(req.target());
}

inline
std::string key_from_http_url(const std::string& url) {
    return util::canonical_url(url);
}

struct CacheEntry {
    using Response = http::response<http::dynamic_body>;

    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;

    // Cached data.
    Response response;
};

} // namespace
