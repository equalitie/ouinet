#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include "../session.h"

namespace ouinet {

struct CacheEntry {
    // Data time stamp, not a date/time on errors.
    boost::posix_time::ptime time_stamp;

    // Cached data.
    Session response;
};

} // namespace
