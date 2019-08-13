#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast.hpp>
#include "../namespaces.h"
#include "../util.h"
#include "../session.h"

namespace ouinet {

template <class Request>
inline
std::string key_from_http_req(const Request& req) {
    assert(req.target().starts_with("http"));
    // The key is currently the canonical URL itself.
    return util::canonical_url(req.target());
}

// Uses of this function may be an indication that request information is missing,
// which could cause problems if at a later point we want to
// take other request parameters to compute cache index keys.
inline
std::string key_from_http_url(const std::string& url) {
    beast::string_view sw(url);
    assert(sw.starts_with("http"));
    return util::canonical_url(url);
}

template <class Key>
inline
Key uri_from_key(Key&& key) {
    // The key is currently the canonical URI itself.
    return key;
}

struct CacheEntry {
    using Response = Session;

    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;

    // Cached data.
    Response response;
};

} // namespace
