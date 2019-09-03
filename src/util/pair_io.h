#pragma once

#include <ostream>
#include <utility>

namespace std {

template<class T1, class T2>
std::ostream& operator<<(std::ostream& os, const std::pair<T1, T2>& p)
{
    return os << "(" << p.first << ", " << p.second << ")";
}

}
