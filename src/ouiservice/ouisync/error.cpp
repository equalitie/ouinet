#include "error.h"

namespace ouinet::ouisync_service {

const boost::system::error_category& error_category() {
    static ErrorCategory instance;
    return instance;
}

} // namespace ouinet::ouisync_service
