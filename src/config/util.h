#pragma once

#include <boost/optional.hpp>
#include <vector>
#include <boost/program_options/variables_map.hpp>
#include "util/str.h"

namespace ouinet {

template <class... Args>
inline
std::runtime_error error(Args && ...args) {
    return std::runtime_error(util::str(std::forward<Args>(args)...));
}

// Helper to avoid writing the name of the option twice.
template<typename T>
static boost::optional<T> as_optional(const boost::program_options::variables_map& vm, const char* name) {
    auto v = vm[name];

    if (v.empty()) {
        return boost::none;
    } else {
        return v.as<T>();
    }
}

// Helper for extracting multiple values
template<typename T>
static std::vector<T> as_vector(const boost::program_options::variables_map& vm, const char* name) {
    auto v = vm[name];

    if (v.empty()) {
        return std::vector<T>();
    } else {
        return v.as<std::vector<T>>();
    }
}

} // namespace
