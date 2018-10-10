#include "sha1.h"

extern "C" {
#include "gcrypt.h"
}

using namespace std;

namespace ouinet { namespace util { namespace sha1_detail {

struct Sha1 {
    ::gcry_md_hd_t impl;
};

size_t size_of_Sha1() { return sizeof(Sha1); }

Sha1* init(void* mem) {
    Sha1* digest = new (mem) Sha1();

    if (::gcry_md_open(&digest->impl, ::gcry_md_algos::GCRY_MD_SHA1, 0)) {
        throw std::runtime_error("Failed to initialize SHA1");
    }

    return digest;
}

void update(Sha1* digest, const void* buffer, size_t size)
{
    ::gcry_md_write(digest->impl, buffer, size);
}

std::array<uint8_t, 20> close(Sha1* digest)
{
    uint8_t *digest_buffer = ::gcry_md_read( digest->impl
                                           , ::gcry_md_algos::GCRY_MD_SHA1);

    std::array<uint8_t, 20> result;

    memcpy(result.data(), digest_buffer, result.size());

    ::gcry_md_close(digest->impl);

    return result;
}

}}} // namespace
