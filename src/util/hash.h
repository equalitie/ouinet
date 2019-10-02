#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/utility/string_view.hpp>

namespace ouinet { namespace util {

enum class hash_algorithm {
    sha1,
    sha256,
    sha512,
};


namespace hash_detail {

class HashImpl;
struct HashImplDeleter {
    void operator()(HashImpl*);
};

HashImpl* new_hash_impl(hash_algorithm);
void hash_impl_update(HashImpl&, const void*, size_t);
uint8_t* hash_impl_close(HashImpl&);

} // namespace hash_detail


/* Templated class to support running hashes.
 *
 * You may call `update` several times to feed the hash function with new
 * data.  When you are done, you may call the `close` function, which returns
 * the resulting digest as an array of bytes.
 */
template<hash_algorithm ALGORITHM, size_t DIGEST_LENGTH>
class Hash {
public:
    using digest_type = std::array<uint8_t, DIGEST_LENGTH>;

    Hash() : impl(hash_detail::new_hash_impl(ALGORITHM)) {};

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

    inline void update(boost::asio::const_buffer data)
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
        auto digest_buffer = hash_detail::hash_impl_close(*impl);

        digest_type result;
        std::memcpy(result.data(), digest_buffer, result.size());
        return result;
    }

private:
    std::unique_ptr<hash_detail::HashImpl, hash_detail::HashImplDeleter> impl;

    inline void update(const void* buffer, size_t size)
    {
        hash_detail::hash_impl_update(*impl, buffer, size);
    }
};

using SHA1 = Hash<hash_algorithm::sha1, 20>;
using SHA256 = Hash<hash_algorithm::sha256, 32>;
using SHA512 = Hash<hash_algorithm::sha512, 64>;


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

template<class Arg, class... Rest>
inline
SHA512::digest_type sha512_digest(const Arg& arg, const Rest&... rest) {
    SHA512 hash;
    return hash_detail::digest(hash, arg, rest...);
}

}} // namespaces
