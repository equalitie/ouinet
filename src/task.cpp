#include "task.h"
#include "util/async.h"
#include "logger.h"

namespace ouinet::task {

void handle_exception_ptr(std::exception_ptr e) {
    try {
        if (e) std::rethrow_exception(e);
    }
    catch (const Async::Cancelled&) {
        // do nothing
    }
    catch (const std::exception& e) {
        LOG_ERROR("Unhandled exception in coroutine ", e.what());
        throw;
    }
    catch (...) {
        LOG_ERROR("Unhandled exception in coroutine (unknown)");
        throw;
    }
}

} // namespace
