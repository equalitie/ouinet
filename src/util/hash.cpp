#include "hash.h"

extern "C" {
#include "gcrypt.h"
}

namespace ouinet { namespace util { namespace hash_detail {

class HashImpl {
public:
    HashImpl(int algo) : algorithm(algo)
    {
        if (::gcry_md_open(&digest, algorithm, 0))
            throw std::runtime_error("Failed to initialize hash");
    }

    ~HashImpl()
    {
        ::gcry_md_close(digest);
    }

    inline void update(const void* buffer, size_t size)
    {
        ::gcry_md_write(digest, buffer, size);
    }

    inline uint8_t* close()
    {
        return ::gcry_md_read(digest, algorithm);
    }

private:
    int algorithm;
    ::gcry_md_hd_t digest;
};

void
HashImplDeleter::operator()(HashImpl* hi)
{
    delete hi;
}

constexpr int
hash_algo(hash_algorithm ha) {
    switch (ha) {
        case hash_algorithm::sha1:
            return ::gcry_md_algos::GCRY_MD_SHA1;
        case hash_algorithm::sha256:
            return ::gcry_md_algos::GCRY_MD_SHA256;
        case hash_algorithm::sha512:
            return ::gcry_md_algos::GCRY_MD_SHA512;
        default:
            return -1;
    }
}

HashImpl*
new_hash_impl(hash_algorithm ha)
{
    return new HashImpl(hash_algo(ha));
}

void
hash_impl_update(HashImpl& hi, const void* buffer, size_t size)
{
    hi.update(buffer, size);
}

uint8_t* hash_impl_close(HashImpl& hi)
{
    return hi.close();
}

}}} // namespaces
