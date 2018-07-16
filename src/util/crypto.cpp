#include "crypto.h"

#include <exception>
#include <vector>

extern "C" {
#include "gcrypt.h"
}

namespace ouinet {
namespace util {

std::string random(unsigned int size)
{
    std::vector<char> buffer(size, '\0');
    ::gcry_create_nonce(buffer.data(), size);
    return std::string(buffer.data(), buffer.size());
}

static std::array<uint8_t, 20> sha1(const void* buffer, size_t size)
{
    ::gcry_md_hd_t digest;

    if (::gcry_md_open(&digest, ::gcry_md_algos::GCRY_MD_SHA1, 0)) {
        throw std::exception();
    }

    ::gcry_md_write(digest, buffer, size);
    unsigned char *digest_buffer = ::gcry_md_read(digest, ::gcry_md_algos::GCRY_MD_SHA1);

    std::array<uint8_t, 20> result;
    memcpy(result.data(), digest_buffer, result.size());

    ::gcry_md_close(digest);
    return result;
}

std::array<uint8_t, 20> sha1(const std::string& data)
{
    return sha1(data.data(), data.size());
}

std::array<uint8_t, 20> sha1(const std::vector<unsigned char>& data)
{
    return sha1(data.data(), data.size());
}

} // util namespace
} // ouinet namespace
