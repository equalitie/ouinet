#pragma once

#include <string>

namespace ouinet::util::random {

// NOTE: These are not cryptographically safe.

void data(void*, size_t);
std::string string(size_t size);

template<typename N /* e.g. uint64_t */>
inline N number()
{
    N ret;
    data(reinterpret_cast<char*>(&ret), sizeof(N));
    return ret;
}

} // namespace
