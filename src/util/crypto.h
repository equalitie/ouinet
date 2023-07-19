#pragma once

#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>
#include <array>
#include <stdint.h>

/*
 * Forward declarations for opaque libgcrypt data structures.
 */
struct gcry_sexp;
typedef gcry_sexp* gcry_sexp_t;

namespace ouinet {
namespace util {

void crypto_init();

class Ed25519PublicKey {
    public:
    static const size_t key_size = 32;
    static const size_t sig_size = 64;
    using key_array_t = std::array<uint8_t, key_size>;
    using sig_array_t = std::array<uint8_t, sig_size>;

    Ed25519PublicKey(key_array_t key = {});
    ~Ed25519PublicKey();

    Ed25519PublicKey(const Ed25519PublicKey& other);
    Ed25519PublicKey(Ed25519PublicKey&& other);
    Ed25519PublicKey& operator=(const Ed25519PublicKey& other);
    Ed25519PublicKey& operator=(Ed25519PublicKey&& other);

    key_array_t serialize() const;

    bool verify(const std::string& data, const sig_array_t& signature) const;

    static
    boost::optional<Ed25519PublicKey> from_hex(boost::string_view);

    private:
    ::gcry_sexp_t _public_key;
};

class Ed25519PrivateKey {
    public:
    static const size_t key_size = 32;
    static const size_t sig_size = Ed25519PublicKey::sig_size;
    using key_array_t = std::array<uint8_t, key_size>;
    using sig_array_t = Ed25519PublicKey::sig_array_t;

    Ed25519PrivateKey(key_array_t key = {});
    ~Ed25519PrivateKey();

    Ed25519PrivateKey(const Ed25519PrivateKey& other);
    Ed25519PrivateKey(Ed25519PrivateKey&& other);
    Ed25519PrivateKey& operator=(const Ed25519PrivateKey& other);
    Ed25519PrivateKey& operator=(Ed25519PrivateKey&& other);

    key_array_t serialize() const;
    Ed25519PublicKey public_key() const;

    static Ed25519PrivateKey generate();

    sig_array_t sign(boost::string_view data) const;

    template<size_t N>
    sig_array_t sign(const std::array<uint8_t, N>& a) const {
        return sign(boost::string_view(reinterpret_cast<const char*>(a.data()), N));
    }

    static
    boost::optional<Ed25519PrivateKey> from_hex(boost::string_view);

    private:
    ::gcry_sexp_t _private_key;
};

std::ostream& operator<<(std::ostream&, const Ed25519PublicKey&);
std::ostream& operator<<(std::ostream&, const Ed25519PrivateKey&);
std::istream& operator>>(std::istream&, Ed25519PublicKey&);
std::istream& operator>>(std::istream&, Ed25519PrivateKey&);

struct Ed25519 {
    using PrivateKey = Ed25519PrivateKey;
    using PublicKey  = Ed25519PublicKey;
};

} // util namespace
} // ouinet namespace
