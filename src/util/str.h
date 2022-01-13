#pragma once

#include <sstream>

#include <boost/system/error_code.hpp>

namespace ouinet { namespace util {

inline
void arg_to_stream(std::ostream& s, boost::system::error_code ec) {
    // Unfortunately, defining this for this particular type
    // in a separate header (to keep this one clean)
    // does not seem to work reliably.
    s << '"' << ec.message() << '"';
}

template<class Arg>
inline
void arg_to_stream(std::ostream& s, Arg&& arg) {
    // This allows overriding the format of a given type
    // without having to override `operator<<`.
    s << arg;
}

inline
void args_to_stream(std::ostream&) { }

template<class Arg, class... Args>
inline
void args_to_stream(std::ostream& s, Arg&& arg, Args&&... args) {
    arg_to_stream(s, std::forward<Arg>(arg));
    args_to_stream(s, std::forward<Args>(args)...);
}

template<class... Args>
inline
std::string str(Args&&... args) {
    std::ostringstream ss;
    args_to_stream(ss, std::forward<Args>(args)...);
    return ss.str();
}

}} // namespaces
