#pragma once

#include <array>
#include <string>
#include <vector>

/*
 * Forward declarations for opaque libgcrypt data structures.
 */
struct gcry_sexp;
typedef gcry_sexp* gcry_sexp_t;

namespace ouinet {
namespace util {

std::string random(unsigned int size);

std::array<uint8_t, 20> sha1(const std::string& data);
std::array<uint8_t, 20> sha1(const std::vector<unsigned char>& data);

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

    private:
    ::gcry_sexp_t _public_key;
};

class Ed25519PrivateKey {
    public:
    Ed25519PrivateKey(const std::array<uint8_t, 32>& key);
    ~Ed25519PrivateKey();

    Ed25519PrivateKey(const Ed25519PrivateKey& other);
    Ed25519PrivateKey(Ed25519PrivateKey&& other);
    Ed25519PrivateKey& operator=(const Ed25519PrivateKey& other);
    Ed25519PrivateKey& operator=(Ed25519PrivateKey&& other);

    std::array<uint8_t, 32> serialize() const;
    Ed25519PublicKey public_key() const;

    static Ed25519PrivateKey generate();

    std::array<uint8_t, 64> sign(const std::string& data) const;

    private:
    ::gcry_sexp_t _private_key;
};

} // util namespace
} // ouinet namespace
