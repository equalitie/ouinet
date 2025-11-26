#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>
#include "namespaces.h"
#include "logger.h"

namespace ouinet { namespace task {

template<
    typename Executor,
    typename Function
    >
void spawn_detached(Executor&& exec, Function&& func) {
#if BOOST_VERSION >= 108000
    asio::spawn(
        std::forward<Executor>(exec),
        std::forward<Function>(func),
        [] (std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
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
        });
#else
    asio::spawn(std::forward<Executor>(exec), std::forward<Function>(func));
#endif
}

}} // ouinet::task
