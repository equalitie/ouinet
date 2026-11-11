#pragma once

#include "api.h"
#include "util/trace.h"
#include <boost/asio/is_executor.hpp>
#include <boost/asio/spawn.hpp>
#include <source_location>

namespace ouinet::task {

OUINET_COMMON_API
void handle_exception_ptr(std::exception_ptr, std::source_location,
                          std::optional<Trace> trace);

template <typename Executor, typename Function>
void spawn_detached(
    Executor &&exec, Function &&func, std::optional<Trace> trace = {},
    std::source_location location = std::source_location::current()) {
#if BOOST_VERSION >= 108000
  boost::asio::spawn(std::forward<Executor>(exec), std::forward<Function>(func),
                     [location = std::move(location),
                      trace = std::move(trace)](std::exception_ptr e) {
                       handle_exception_ptr(e, std::move(location),
                                            std::move(trace));
                     });
#else
  boost::asio::spawn(std::forward<Executor>(exec),
                     std::forward<Function>(func));
#endif
}

} // namespace ouinet::task
