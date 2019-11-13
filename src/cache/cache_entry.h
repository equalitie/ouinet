#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast.hpp>
#include <boost/optional.hpp>
#include "../namespaces.h"
#include "../util.h"
#include "../session.h"

namespace ouinet {

// If a key cannot be correctly derived from the request,
// return none.
template <class Request>
inline
boost::optional<std::string> key_from_http_req(const Request& req) {
    if (!req.target().starts_with("http")) return {};
    // The key is currently the canonical URL itself.
    return util::canonical_url(req.target());
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
