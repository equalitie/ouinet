#pragma once

#include <string>
#include "api.h"

namespace ouinet::util::random {

// NOTE: These are not cryptographically safe.

OUINET_COMMON_API void data(void*, size_t);
OUINET_COMMON_API std::string string(size_t size);
OUINET_COMMON_API std::string printable_ascii(size_t size);

template<typename N /* e.g. uint64_t */>
inline N number()
{
    N ret;
    data(reinterpret_cast<char*>(&ret), sizeof(N));
    return ret;
}

// If you get an undefined reference with this one, add
// a template specialization to the cpp file.
template<typename N> N number(N min, N max);

} // namespace
