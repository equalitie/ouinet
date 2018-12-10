#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast.hpp>
#include "../namespaces.h"

namespace ouinet {

template <class Request>
inline
std::string key_from_http_req(const Request& req) {
    return req.target().to_string();  // TODO: canonical
}

struct CacheEntry {
    using Response = http::response<http::dynamic_body>;

    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;

    // Cached data.
    Response response;
};

} // namespace
