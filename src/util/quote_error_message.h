#pragma once

// Include this to represent error codes as a quoted message string
// (e.g. for `ouinet::util::str`).

#include <ostream>

#include <boost/system/error_code.hpp>

namespace ouinet { namespace util {

inline
std::ostream& operator<<(std::ostream& s, boost::system::error_code ec)
{
    s << '"' << ec.message() << '"';
    return s;
}

}} // namespaces


#ifdef __ANDROID__
// For some reason the operator defined above is ignored when building for Android,
// so also place it explicitly in the namespace of the one we try to override.

namespace boost { namespace system {

inline
std::ostream& operator<<(std::ostream& s, error_code ec)
{
    s << '"' << ec.message() << '"';
    return s;
}

}} // namespaces
#endif // ifdef __ANDROID__
