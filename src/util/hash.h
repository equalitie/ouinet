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

    Hash() {}

    static digest_type zero_digest() {
        static bool filled = false;
        static digest_type ret;
        if (!filled) {
            filled = true;
            ret.fill(0);
        }
        return ret;
    }

    inline void update(boost::string_view sv)
    {
        update(sv.data(), sv.size());
    }

    inline void update(const char* c)
    {
        update(boost::string_view(c));
    }

    inline void update(const std::string& data)
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
        if (!impl) impl.reset(hash_detail::new_hash_impl(ALGORITHM));

        auto digest_buffer = hash_detail::hash_impl_close(*impl);

        digest_type result;
        std::memcpy(result.data(), digest_buffer, result.size());

        impl = nullptr;

        return result;
    }

    template<class... Args>
    static
    digest_type digest(Args&&... args)
    {
        Hash hash;
        return digest_impl(hash, std::forward<Args>(args)...);
    }

    static constexpr size_t size() {
        return DIGEST_LENGTH;
    }

private:
    template<class Hash>
    static
    digest_type digest_impl(Hash& hash)
    {
        return hash.close();
    }

    template<class Hash, class Arg, class... Rest>
    static
    digest_type digest_impl(Hash& hash, const Arg& arg, const Rest&... rest)
    {
        hash.update(arg);
        return digest_impl(hash, rest...);
    }

private:
    std::unique_ptr<hash_detail::HashImpl, hash_detail::HashImplDeleter> impl;

    inline void update(const void* buffer, size_t size)
    {
        if (!impl) impl.reset(hash_detail::new_hash_impl(ALGORITHM));
        hash_detail::hash_impl_update(*impl, buffer, size);
    }
};

using SHA1 = Hash<hash_algorithm::sha1, 20>;
using SHA256 = Hash<hash_algorithm::sha256, 32>;
using SHA512 = Hash<hash_algorithm::sha512, 64>;


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
    return SHA1::digest(arg, rest...);
}

template<class Arg, class... Rest>
inline
SHA256::digest_type sha256_digest(const Arg& arg, const Rest&... rest) {
    return SHA256::digest(arg, rest...);
}

template<class Arg, class... Rest>
inline
SHA512::digest_type sha512_digest(const Arg& arg, const Rest&... rest) {
    return SHA512::digest(arg, rest...);
}

}} // namespaces
