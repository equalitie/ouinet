#include "random.h"
#include <random>

namespace ouinet::util::random {

std::random_device g_dev;
std::mt19937 g_rng(g_dev());

inline void data(void* out, size_t size, uint8_t min, uint8_t max)
{
    std::uniform_int_distribution<std::mt19937::result_type> dist(min, max);

    uint8_t* p = reinterpret_cast<uint8_t*>(out);

    for (size_t i = 0; i < size; ++i) {
        *(p++) = dist(g_rng);
    }
}

void data(void* out, size_t size)
{
    data(out, size, 0, 255);
}

std::string string(size_t size)
{
    std::string s(size, '\0');
    data(s.data(), s.size());
    return s;
}

std::string printable_ascii(size_t size) {
    std::string s(size, '\0');
    data(s.data(), s.size(), uint8_t(' '), uint8_t('~'));
    return s;
}

template<typename N> N number(N min, N max) {
    std::uniform_int_distribution<N> dist(min, max);
    return dist(g_rng);
}

// Explicit function template instantiation. Add more as needed.
template size_t number<size_t>(size_t, size_t);

} // namespaces
