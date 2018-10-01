#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast.hpp>
#include "../namespaces.h"

namespace ouinet {

// TODO: This is temporary while a refactor is taking place
struct CacheEntry {
    using Response = http::response<http::dynamic_body>;

    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;
    // Cached data.
    Response response;
};

} // namespace
