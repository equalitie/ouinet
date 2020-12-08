#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>
#include "response_part.h"

namespace ouinet {

// TODO(peter): Currently this class holds only the timestamp, but I expect in
// the future for it to contain all variables that get parsed when signature
// verification takes place.
class SignedHead : public http_response::Head {
public:

    SignedHead() = default;

    static SignedHead parse(http_response::Head raw_head, sys::error_code&);

    boost::posix_time::ptime time_stamp() const { return _time_stamp; }

private:
    SignedHead(http_response::Head raw_head)
        : http_response::Head(std::move(raw_head))
    {}

private:
    boost::posix_time::ptime _time_stamp;
};

} // namespace
