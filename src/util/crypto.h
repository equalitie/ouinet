#pragma once

#include <boost/optional.hpp>
#include "sha1.h"

/*
 * Forward declarations for opaque libgcrypt data structures.
 */
struct gcry_sexp;
typedef gcry_sexp* gcry_sexp_t;

namespace ouinet {
namespace util {

void crypto_init();

std::string random(unsigned int size);

class Ed25519PublicKey {
    public:
    Ed25519PublicKey(std::array<uint8_t, 32> key = {});
    ~Ed25519PublicKey();

    Ed25519PublicKey(const Ed25519PublicKey& other);
    Ed25519PublicKey(Ed25519PublicKey&& other);
    Ed25519PublicKey& operator=(const Ed25519PublicKey& other);
    Ed25519PublicKey& operator=(Ed25519PublicKey&& other);

    std::array<uint8_t, 32> serialize() const;

    bool verify(const std::string& data, const std::array<uint8_t, 64>& signature) const;

    static
    boost::optional<Ed25519PublicKey> from_hex(boost::string_view);

    private:
    ::gcry_sexp_t _public_key;
};

class Ed25519PrivateKey {
    public:
    Ed25519PrivateKey(std::array<uint8_t, 32> key = {});
    ~Ed25519PrivateKey();

    Ed25519PrivateKey(const Ed25519PrivateKey& other);
    Ed25519PrivateKey(Ed25519PrivateKey&& other);
    Ed25519PrivateKey& operator=(const Ed25519PrivateKey& other);
    Ed25519PrivateKey& operator=(Ed25519PrivateKey&& other);

    std::array<uint8_t, 32> serialize() const;
    Ed25519PublicKey public_key() const;

    static Ed25519PrivateKey generate();

    std::array<uint8_t, 64> sign(const std::string& data) const;

    static
    boost::optional<Ed25519PrivateKey> from_hex(boost::string_view);

    private:
    ::gcry_sexp_t _private_key;
};

std::ostream& operator<<(std::ostream&, const Ed25519PublicKey&);
std::ostream& operator<<(std::ostream&, const Ed25519PrivateKey&);
std::istream& operator>>(std::istream&, Ed25519PublicKey&);
std::istream& operator>>(std::istream&, Ed25519PrivateKey&);

} // util namespace
} // ouinet namespace
