#pragma once

#include <ostream>
#include <map>
#include "pair_io.h"

namespace std {

template<class K, class V>
std::ostream& operator<<(std::ostream& os, const std::map<K, V>& map)
{
    os << "{";
    for (auto i = map.begin(); i != map.end();) {
        os << *i;
        if (++i != map.end()) { os << ","; }
    }
    return os << "}";
}

}
