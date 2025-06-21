#pragma once

#include <boost/version.hpp>

#if BOOST_VERSION >= 107400
#   include <boost/asio/any_io_executor.hpp>
#else
#   include <boost/asio/executor.hpp>
#endif

#include "../namespaces.h"

namespace ouinet { namespace util {
#if BOOST_VERSION >= 107400
        using AsioExecutor = boost::asio::any_io_executor;
#else
        using AsioExecutor = boost::asio::executor;
#endif
}} // ouinet::util namespace
