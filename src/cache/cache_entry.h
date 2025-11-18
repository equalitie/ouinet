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
inline
std::optional<std::string> key_from_http_req(const http::request_header<>& req) {
    auto url = util::Url::from(req.target());
    if (!url) return {};
    auto key = util::canonical_url(std::move(*url));
    if (key.empty()) return {};
    return key;
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
