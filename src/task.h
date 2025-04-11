#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>
#include "namespaces.h"

namespace ouinet { namespace task {

template<
    typename Executor,
    typename Function
    >
void spawn_detached(Executor&& exec, Function&& func) {
#if BOOST_VERSION >= 108000
    asio::spawn(std::forward<Executor>(exec), std::forward<Function>(func), asio::detached);
#else
    asio::spawn(std::forward<Executor>(exec), std::forward<Function>(func));
#endif
}

}} // ouinet::task
