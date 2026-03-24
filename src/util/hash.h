#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/utility/string_view.hpp>
#include <openssl/evp.h>

namespace ouinet::util {

namespace algo {
    struct Sha1 {
        static const size_t digest_size = 20;
        static const EVP_MD* initializer() { return EVP_sha1(); }
    };
    struct Sha256 {
        static const size_t digest_size = 32;
        static const EVP_MD* initializer() { return EVP_sha256(); }
    };
    struct Sha512 {
        static const size_t digest_size = 64;
        static const EVP_MD* initializer() { return EVP_sha512(); }
    };
}

/* Templated class to support running hashes.
 *
 * You may call `update` several times to feed the hash function with new
 * data.  When you are done, you may call the `close` function, which returns
 * the resulting digest as an array of bytes.
 */
template<typename Algo> class Hash {
public:
    using digest_type = std::array<uint8_t, Algo::digest_size>;

    Hash() {}

    // TODO
    Hash(Hash const&) = delete;
    Hash(Hash &&) = delete;
    Hash& operator=(Hash const&) = delete;
    Hash& operator=(Hash &&) = delete;

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

    inline void update(std::string_view sv)
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
        lazy_init();

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;

        if (EVP_DigestFinal_ex(_ctx, hash, &hash_len) != 1) {
            throw std::runtime_error("failed to finalize hash");
        }

        digest_type result;

        if (hash_len != result.size()) {
            throw std::runtime_error("invalid hash size");
        }

        std::memcpy(result.data(), hash, hash_len);

        EVP_MD_CTX_free(_ctx);
        _ctx = nullptr;

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
        return Algo::digest_size;
    }

    ~Hash() {
        if (_ctx) EVP_MD_CTX_free(_ctx);
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
    EVP_MD_CTX* _ctx = nullptr;

    void update(const void* buffer, size_t size)
    {
        lazy_init();
        if (EVP_DigestUpdate(_ctx, buffer, size) != 1) {
            throw std::runtime_error("failed to update hash context");
        }
    }

    void lazy_init() {
        if (_ctx) return;
        _ctx = EVP_MD_CTX_new();
        if (!_ctx) throw std::runtime_error("failed to create hash context");
        if (EVP_DigestInit_ex(_ctx, Algo::initializer(), NULL) != 1) {
            throw std::runtime_error("failed to initialie hash context");
        }
    }
};

using SHA1 = Hash<algo::Sha1>;
using SHA256 = Hash<algo::Sha256>;
using SHA512 = Hash<algo::Sha512>;


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

} // namespaces
