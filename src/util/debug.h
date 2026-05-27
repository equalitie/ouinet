#pragma once

#include <ostream>
#include <expected>

namespace ouinet {

// Adapter for debug printing of various types.
//
// Usage: `ostream << debug(my_value)`;
template<typename T>
struct Debug {
    const T& inner;
};

template<typename T>
Debug<T> debug(const T& inner) {
    return Debug { inner };
}

template<typename T, typename E>
std::ostream& operator<<(std::ostream& os, const Debug<std::expected<T, E>>& d) {
    if (d.inner.has_value()) {
        os << "ok(";

        if constexpr (!std::is_void_v<T>) {
            os << d.inner.value();
        }
    } else {
        os << "error(" << d.inner.error();
    }

    return os << ")";
}

} // namespace ouinet
