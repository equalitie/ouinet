#pragma once

#include <sstream>

namespace ouinet { namespace util {

inline
void args_to_stream(std::ostream&) { }

template<class Arg, class... Args>
inline
void args_to_stream(std::ostream& s, Arg&& arg, Args&&... args) {
    s << arg;
    args_to_stream(s, std::forward<Args>(args)...);
}

template<class... Args>
inline
std::string str(Args&&... args) {
    std::stringstream ss;
    args_to_stream(ss, std::forward<Args>(args)...);
    return ss.str();
}

}} // namespaces
