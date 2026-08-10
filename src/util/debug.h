#pragma once

#include <ostream>
#include <expected>
#include <map>
#include <set>
#include <optional>

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

// std::expected
template<typename T, typename E>
std::ostream& operator<<(std::ostream& os, const Debug<std::expected<T, E>>& d) {
    if (d.inner.has_value()) {
        os << "ok(";

        if constexpr (!std::is_void_v<T>) {
            os << debug(d.inner.value());
        }
    } else {
        os << "error(";

       if constexpr (std::is_same_v<E, boost::system::error_code>) {
           os << d.inner.error().what();
       } else {
           os << debug(d.inner.error());
       }
    }

    return os << ")";
}

// std::set
template<typename T>
std::ostream& operator<<(std::ostream& os, const Debug<std::set<T>>& d) {
    os << "{";
    bool is_first = true;
    for (auto& v : d.inner) {
        if (is_first) {
            os << debug(v);
            is_first = false;
        } else {
            os << ", " << debug(v);
        }
    }
    return os << "}";
}

// std::map
template<typename K, typename V>
std::ostream& operator<<(std::ostream& os, const Debug<std::map<K, V>>& d) {
    os << "{";
    bool is_first = true;
    for (auto& v : d.inner) {
        if (is_first) {
            is_first = false;
        } else {
            os << ", ";
        }

        os << debug(v.first) << ": " << debug(v.second);
    }
    return os << "}";
}

// std::vector
template<typename T>
std::ostream& operator<<(std::ostream& os, const Debug<std::vector<T>>& d) {
    os << "{";
    bool is_first = true;
    for (auto& v : d.inner) {
        if (is_first) {
            os << debug(v);
            is_first = false;
        } else {
            os << ", " << debug(v);
        }
    }
    return os << "}";
}

// std::optional
template<typename T>
std::ostream& operator<<(std::ostream& os, const Debug<std::optional<T>>& d) {
    if (d.inner) {
        return os << "some(" << debug(*d.inner) << ")";
    } else {
        return os << "none";
    }
}

// Fallback
template<typename T>
std::ostream& operator<<(std::ostream& os, const Debug<T>& d) {
    return os << d.inner;
}


} // namespace ouinet
