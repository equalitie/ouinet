#include "random.h"

extern "C" {
#include "gcrypt.h"
}

namespace ouinet { namespace util { namespace random {

void data(void* data, unsigned int size)
{
    ::gcry_create_nonce(data, size);
}

std::string string(unsigned int size)
{
    std::string s(size, '\0');
    ::gcry_create_nonce((void*) s.data(), size);
    return s;
}

}}} // namespaces

