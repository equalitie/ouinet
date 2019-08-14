#pragma once

#include <ostream>
#include <set>

namespace std {

template<class T>
std::ostream& operator<<(std::ostream& os, const std::set<T>& set)
{
    os << "{";
    for (auto i = set.begin(); i != set.end();) {
        os << *i;
        if (++i != set.end()) { os << ","; }
    }
    return os << "}";
}

}
