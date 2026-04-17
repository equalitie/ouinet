#include "random.h"
#include <random>

namespace ouinet::util::random {

std::random_device g_dev;
std::mt19937 g_rng(g_dev());
 
void data(void* data, size_t size)
{
    std::uniform_int_distribution<std::mt19937::result_type> dist(0,255);

    uint8_t* p = reinterpret_cast<uint8_t*>(data);

    for (size_t i = 0; i < size; ++i) {
        *(p++) = dist(g_rng);
    }
}

std::string string(size_t size)
{
    std::string s(size, '\0');
    data(s.data(), s.size());
    return s;
}

} // namespaces
