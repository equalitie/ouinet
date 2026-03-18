#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include "../session.h"

namespace ouinet {

struct CacheEntry {
    using Response = Session;

    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;

    // Cached data.
    Response response;
};

} // namespace
