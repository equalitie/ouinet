#pragma once

#include <boost/asio/is_executor.hpp>
#include <boost/asio/spawn.hpp>
#include <source_location>
#include "api.h"

namespace ouinet::task {

OUINET_COMMON_API
void handle_exception_ptr(std::exception_ptr, std::source_location);

template<
    typename Executor,
    typename Function
>
void spawn_detached(
    Executor&& exec,
    Function&& func,
    std::source_location location = std::source_location::current()
) {
#if BOOST_VERSION >= 108000
    boost::asio::spawn(
        std::forward<Executor>(exec),
        std::forward<Function>(func),
        [location = std::move(location)] (std::exception_ptr e) {
            handle_exception_ptr(e, std::move(location));
        }
    );
#else
    boost::asio::spawn(std::forward<Executor>(exec), std::forward<Function>(func));
#endif
}

} // namespace
