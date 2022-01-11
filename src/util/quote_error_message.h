#pragma once

// Include this to represent error codes as a quoted message string
// (e.g. for `ouinet::util::str`).

#include <sstream>

#include <boost/system/error_code.hpp>

namespace ouinet { namespace util {

inline
std::ostream& operator<<(std::ostream& s, const boost::system::error_code& ec) {
    return s << '"' << ec.message() << '"';
}

}} // namespaces
