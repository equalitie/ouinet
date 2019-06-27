#pragma once

#include <array>
#include <vector>
#include <string>

#include <boost/utility/string_view.hpp>

extern "C" {
#include "gcrypt.h"
}

namespace ouinet { namespace util {

/* Templated class to support running hashes.
 *
 * You may call `update` several times to feed the hash function with new
 * data.  When you are done, you may call the `close` function, which returns
 * the resulting digest as an array of bytes.
 */
template<int ALGORITHM, size_t DIGEST_LENGTH>
class Hash {
public:
    using digest_type = std::array<uint8_t, DIGEST_LENGTH>;

    Hash()
    {
        digest = new (mem) md_handle();
        if (::gcry_md_open(&digest->impl, ALGORITHM, 0))
            throw std::runtime_error("Failed to initialize hash");
    }

    ~Hash()
    {
        ::gcry_md_close(digest->impl);
    }

    inline void update(boost::string_view sv)
    {
        update(sv.data(), sv.size());
    }

    inline void update(const char* c)
    {
        update(boost::string_view(c));
    }

    inline void update(std::string& data)
    {
        update(data.data(), data.size());
    }

    inline void update(const std::vector<unsigned char>& data)
    {
        update(data.data(), data.size());
    }

    template<size_t N>
    inline void update(const std::array<uint8_t, N>& data)
    {
        update(data.data(), N);
    }

    inline digest_type close()
    {
        uint8_t* digest_buffer = ::gcry_md_read(digest->impl, ALGORITHM);

        digest_type result;
        memcpy(result.data(), digest_buffer, result.size());
        return result;
    }

private:
    struct md_handle {
        ::gcry_md_hd_t impl;
    };

    uint8_t mem[DIGEST_LENGTH];
    md_handle* digest;

    inline void update(const void* buffer, size_t size) {
        ::gcry_md_write(digest->impl, buffer, size);
    }
};

using SHA1 = Hash<::gcry_md_algos::GCRY_MD_SHA1, 20>;
using SHA256 = Hash<::gcry_md_algos::GCRY_MD_SHA256, 32>;


namespace hash_detail {

template<class Hash>
inline
typename Hash::digest_type digest(Hash& hash) {
    return hash.close();
}

template<class Hash, class Arg, class... Rest>
inline
typename Hash::digest_type digest(Hash& hash, const Arg& arg, const Rest&... rest) {
    hash.update(arg);
    return digest(hash, rest...);
}

} // namespace hash_detail


/* Utility functions to get the hash of a set of strings.
 * The result is returned as an array of bytes.
 * Usage:
 *
 *     auto hash = sha1_digest(string("hello world"));
 *
 * Or:
 *
 *     auto hash = sha1_digest(boost::string_view("hello world"));
 *
 * Or:
 *
 *     string s = "hello ";
 *     string_view sv = "world";
 *     auto hash = sha1_digest(s + sv.to_string());
 *
 * Or:
 *
 *     string s = "hello ";
 *     string_view sv = "world";
 *     auto hash = sha1_digest(hello, world);
 *
 * Note that the last one is more efficient than the one before because it
 * does not allocate a new string as a result of applying `operator+`.
 */

template<class Arg, class... Rest>
inline
SHA1::digest_type sha1_digest(const Arg& arg, const Rest&... rest) {
    SHA1 hash;
    return hash_detail::digest(hash, arg, rest...);
}

template<class Arg, class... Rest>
inline
SHA256::digest_type sha256_digest(const Arg& arg, const Rest&... rest) {
    SHA256 hash;
    return hash_detail::digest(hash, arg, rest...);
}

}} // namespaces
