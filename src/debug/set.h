#pragma once

#include <ostream>
#include <set>

namespace ouinet {

template<class V> struct DebugStdSet {
    const std::set<V>& set;

    DebugStdSet(const std::set<V>& set) : set(set) {}

    friend std::ostream& operator<<(std::ostream& os, const DebugStdSet<V>& d) {
        os << "{";
        bool is_first = true;
        for (auto& v : d.set) {
            if (is_first) {
                os << v;
                is_first = false;
            } else {
                os << ", " << v;
            }
        }
        return os << "}";
    }
};

template<class V>
DebugStdSet<V> debug(const std::set<V>& set) {
    return DebugStdSet<V>(set);
}

} // namespace
