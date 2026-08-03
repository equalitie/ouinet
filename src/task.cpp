#include "task.h"
#include "util/async.h"
#include "logger.h"
#include <source_location>

namespace ouinet::task {

void handle_exception_ptr(std::exception_ptr e, std::source_location location) {
    try {
        if (e) std::rethrow_exception(e);
    }
    catch (const Async::Cancelled&) {
        // do nothing
    }
    catch (const std::exception& e) {
        LOG_ERROR(
            "Unhandled exception in coroutine in ", location.file_name(),
            ":", location.line(),
            ":", location.column(),
            ": ", e.what()
        );
        throw;
    }
    catch (...) {
        LOG_ERROR(
            "Unhandled exception in coroutine in ", location.file_name(),
            ":", location.line(),
            ":", location.column(),
            ": unknown"
        );
        throw;
    }
}

} // namespace
