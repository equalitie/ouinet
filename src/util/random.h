#pragma once

#include <string>

namespace ouinet::util::random {

void data(void*, unsigned int);
std::string string(unsigned int size);

template<typename N /* e.g. uint64_t */>
inline N number()
{
    N ret;
    data(reinterpret_cast<char*>(&ret), sizeof(N));
    return ret;
}

} // namespace
