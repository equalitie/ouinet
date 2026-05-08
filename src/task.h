#pragma once

#include <boost/asio/spawn.hpp>
#include "namespaces.h"

namespace ouinet { namespace task {

void handle_exception_ptr(std::exception_ptr);

template<
    typename Executor,
    typename Function
>
void spawn_detached(Executor&& exec, Function&& func) {
#if BOOST_VERSION >= 108000
    asio::spawn(
        std::forward<Executor>(exec),
        std::forward<Function>(func),
        handle_exception_ptr);
#else
    asio::spawn(std::forward<Executor>(exec), std::forward<Function>(func));
#endif
}

}} // ouinet::task
