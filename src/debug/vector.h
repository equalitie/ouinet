#pragma once

#include <ostream>
#include <vector>

namespace ouinet {

template<class V> struct DebugStdVector {
    const std::vector<V>& vec;

    DebugStdVector(const std::vector<V>& vec) : vec(vec) {}

    friend std::ostream& operator<<(std::ostream& os, const DebugStdVector<V>& d) {
        os << "{";
        bool is_first = true;
        for (auto& v : d.vec) {
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
DebugStdVector<V> debug(std::vector<V>& v) {
    return DebugStdVector<V>(v);
}

} // namespace
