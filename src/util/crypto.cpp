#include "crypto.h"
#include "bytes.h"

#include <cassert>
#include <exception>
#include <vector>

extern "C" {
#include "gcrypt.h"
}

#include <iostream>

namespace ouinet {
namespace util {

void crypto_init()
{
    if (!::gcry_check_version(GCRYPT_VERSION)) {
        throw std::runtime_error("Error: Incompatible gcrypt version");
    }
}

std::string random(unsigned int size)
{
    std::vector<char> buffer(size, '\0');
    ::gcry_create_nonce(buffer.data(), size);
    return std::string(buffer.data(), buffer.size());
}

Ed25519PublicKey::Ed25519PublicKey(std::array<uint8_t, 32> key):
    _public_key(nullptr)
{
    if (::gcry_sexp_build(&_public_key, NULL, "(public-key (ecc (curve Ed25519) (flags eddsa) (q %b)))", key.size(), key.data())) {
        throw std::exception();
    }
}

Ed25519PublicKey::~Ed25519PublicKey()
{
    if (_public_key) {
        ::gcry_sexp_release(_public_key);
        _public_key = nullptr;
    }
}

Ed25519PublicKey::Ed25519PublicKey(const Ed25519PublicKey& other):
    _public_key(nullptr)
{
    (*this) = other;
}

Ed25519PublicKey::Ed25519PublicKey(Ed25519PublicKey&& other):
    _public_key(nullptr)
{
    (*this) = other;
}

boost::optional<Ed25519PublicKey>
Ed25519PublicKey::from_hex(boost::string_view hex)
{
    if (hex.size() != 64 || !util::bytes::is_hex(hex)) {
        return boost::none;
    }

    return Ed25519PublicKey(
            util::bytes::to_array<uint8_t, 32>(util::bytes::from_hex(hex)));
}

Ed25519PublicKey& Ed25519PublicKey::operator=(const Ed25519PublicKey& other)
{
    if (this != &other) {
        if (_public_key) {
            ::gcry_sexp_release(_public_key);
            _public_key = nullptr;
        }

        if (other._public_key) {
            if (::gcry_sexp_build(&_public_key, NULL, "%S", other._public_key)) {
                _public_key = nullptr;
                throw std::exception();
            }
        }
    }
    return *this;
}

Ed25519PublicKey& Ed25519PublicKey::operator=(Ed25519PublicKey&& other)
{
    if (this != &other) {
        std::swap(_public_key, other._public_key);
    }
    return *this;
}

std::array<uint8_t, 32> Ed25519PublicKey::serialize() const
{
    ::gcry_sexp_t q = ::gcry_sexp_find_token(_public_key, "q", 0);
    if (!q) {
        throw std::exception();
    }
    size_t q_size;
    const char* q_buffer = ::gcry_sexp_nth_data(q, 1, &q_size);
    if (!q_buffer) {
        ::gcry_sexp_release(q);
        throw std::exception();
    }
    std::array<uint8_t, 32> output;
    assert(q_size == output.size());
    memcpy(output.data(), q_buffer, output.size());
    ::gcry_sexp_release(q);
    return output;
}

bool Ed25519PublicKey::verify(const std::string& data, const std::array<uint8_t, 64>& signature) const
{
    ::gcry_sexp_t signature_sexp;
    if (::gcry_sexp_build(&signature_sexp, NULL, "(sig-val (eddsa (r %b)(s %b)))", 32, signature.data(), 32, signature.data() + 32)) {
        throw std::exception();
    }

    ::gcry_sexp_t data_sexp;
    if (::gcry_sexp_build(&data_sexp, NULL, "(data (flags eddsa) (hash-algo sha512) (value %b))", data.size(), data.data())) {
        ::gcry_sexp_release(data_sexp);
        throw std::exception();
    }

    ::gcry_error_t error = gcry_pk_verify(signature_sexp, data_sexp, _public_key);

    ::gcry_sexp_release(data_sexp);
    ::gcry_sexp_release(signature_sexp);

    return error == 0;
}



Ed25519PrivateKey::Ed25519PrivateKey(std::array<uint8_t, 32> key):
    _private_key(nullptr)
{
    if (::gcry_sexp_build(&_private_key, NULL, "(private-key (ecc (curve Ed25519) (flags eddsa) (d %b)))", key.size(), key.data())) {
        throw std::exception();
    }
}

Ed25519PrivateKey::~Ed25519PrivateKey()
{
    if (_private_key) {
        ::gcry_sexp_release(_private_key);
        _private_key = nullptr;
    }
}

Ed25519PrivateKey::Ed25519PrivateKey(const Ed25519PrivateKey& other):
    _private_key(nullptr)
{
    (*this) = other;
}

Ed25519PrivateKey::Ed25519PrivateKey(Ed25519PrivateKey&& other):
    _private_key(nullptr)
{
    (*this) = other;
}

Ed25519PrivateKey& Ed25519PrivateKey::operator=(const Ed25519PrivateKey& other)
{
    if (this != &other) {
        if (_private_key) {
            ::gcry_sexp_release(_private_key);
            _private_key = nullptr;
        }

        if (other._private_key) {
            if (::gcry_sexp_build(&_private_key, NULL, "%S", other._private_key)) {
                _private_key = nullptr;
                throw std::exception();
            }
        }
    }
    return *this;
}

Ed25519PrivateKey& Ed25519PrivateKey::operator=(Ed25519PrivateKey&& other)
{
    if (this != &other) {
        std::swap(_private_key, other._private_key);
    }
    return *this;
}

std::array<uint8_t, 32> Ed25519PrivateKey::serialize() const
{
    ::gcry_sexp_t d = ::gcry_sexp_find_token(_private_key, "d", 0);
    if (!d) {
        throw std::exception();
    }
    size_t d_size;
    const char* d_buffer = ::gcry_sexp_nth_data(d, 1, &d_size);
    if (!d_buffer) {
        ::gcry_sexp_release(d);
        throw std::exception();
    }
    std::array<uint8_t, 32> output;
    assert(d_size == output.size());
    memcpy(output.data(), d_buffer, output.size());
    ::gcry_sexp_release(d);
    return output;
}

boost::optional<Ed25519PrivateKey>
Ed25519PrivateKey::from_hex(boost::string_view hex)
{
    if (hex.size() != 64 || !util::bytes::is_hex(hex)) {
        return boost::none;
    }

    return Ed25519PrivateKey(
            util::bytes::to_array<uint8_t, 32>(util::bytes::from_hex(hex)));
}

Ed25519PublicKey Ed25519PrivateKey::public_key() const
{
    /*
     * This logic is even less well documented than the rest of gcrypt.
     */
    ::gcry_ctx_t public_key_parameters;
    if (::gcry_mpi_ec_new(&public_key_parameters, _private_key, NULL)) {
        throw std::exception();
    }
    ::gcry_sexp_t public_key_sexp;
    if (::gcry_pubkey_get_sexp(&public_key_sexp, GCRY_PK_GET_PUBKEY, public_key_parameters)) {
        ::gcry_ctx_release(public_key_parameters);
        throw std::exception();
    }
    ::gcry_ctx_release(public_key_parameters);

    ::gcry_sexp_t q = ::gcry_sexp_find_token(public_key_sexp, "q", 0);
    if (!q) {
        ::gcry_sexp_release(public_key_sexp);
        throw std::exception();
    }
    ::gcry_sexp_release(public_key_sexp);
    size_t q_size;
    const char* q_buffer = ::gcry_sexp_nth_data(q, 1, &q_size);
    if (!q_buffer) {
        ::gcry_sexp_release(q);
        throw std::exception();
    }
    std::array<uint8_t, 32> public_key;
    assert(q_size == public_key.size());
    memcpy(public_key.data(), q_buffer, public_key.size());
    ::gcry_sexp_release(q);

    return Ed25519PublicKey(public_key);
}

Ed25519PrivateKey Ed25519PrivateKey::generate()
{
    ::gcry_sexp_t generation_parameters;
    if (gcry_sexp_build(&generation_parameters, NULL, "(genkey (ecc (curve Ed25519) (flags eddsa)))")) {
        throw std::exception();
    }

    ::gcry_sexp_t private_key_sexp;
    if (::gcry_pk_genkey(&private_key_sexp, generation_parameters)) {
        ::gcry_sexp_release(generation_parameters);
        throw std::exception();
    }
    ::gcry_sexp_release(generation_parameters);

    ::gcry_sexp_t d = ::gcry_sexp_find_token(private_key_sexp, "d", 0);
    if (!d) {
        ::gcry_sexp_release(private_key_sexp);
        throw std::exception();
    }
    ::gcry_sexp_release(private_key_sexp);
    size_t d_size;
    const char* d_buffer = ::gcry_sexp_nth_data(d, 1, &d_size);
    if (!d_buffer) {
        ::gcry_sexp_release(d);
        throw std::exception();
    }
    std::array<uint8_t, 32> private_key;
    assert(d_size == private_key.size());
    memcpy(private_key.data(), d_buffer, private_key.size());
    ::gcry_sexp_release(d);

    return Ed25519PrivateKey(private_key);
}

std::array<uint8_t, 64> Ed25519PrivateKey::sign(const std::string& data) const
{
    ::gcry_sexp_t data_sexp;
    if (::gcry_sexp_build(&data_sexp, NULL, "(data (flags eddsa) (hash-algo sha512) (value %b))", data.size(), data.data())) {
        throw std::exception();
    }

    ::gcry_sexp_t signature_sexp;
    if (::gcry_pk_sign(&signature_sexp, data_sexp, _private_key)) {
        ::gcry_sexp_release(data_sexp);
        throw std::exception();
    }
    ::gcry_sexp_release(data_sexp);

    ::gcry_sexp_t r_sexp = ::gcry_sexp_find_token(signature_sexp, "r", 0);
    if (!r_sexp) {
        ::gcry_sexp_release(signature_sexp);
        throw std::exception();
    }
    size_t r_size;
    const char* r_buffer = ::gcry_sexp_nth_data(r_sexp, 1, &r_size);
    if (!r_buffer) {
        ::gcry_sexp_release(r_sexp);
        ::gcry_sexp_release(signature_sexp);
        throw std::exception();
    }

    ::gcry_sexp_t s_sexp = ::gcry_sexp_find_token(signature_sexp, "s", 0);
    if (!r_sexp) {
        ::gcry_sexp_release(r_sexp);
        ::gcry_sexp_release(signature_sexp);
        throw std::exception();
    }
    size_t s_size;
    const char* s_buffer = ::gcry_sexp_nth_data(s_sexp, 1, &s_size);
    if (!s_buffer) {
        ::gcry_sexp_release(s_sexp);
        ::gcry_sexp_release(r_sexp);
        ::gcry_sexp_release(signature_sexp);
        throw std::exception();
    }

    ::gcry_sexp_release(signature_sexp);
    std::array<uint8_t, 64> output;
    assert(r_size == 32);
    assert(s_size == 32);
    memcpy(output.data(), r_buffer, 32);
    memcpy(output.data() + 32, s_buffer, 32);
    ::gcry_sexp_release(s_sexp);
    ::gcry_sexp_release(r_sexp);

    return output;
}

std::ostream& operator<<(std::ostream& os, const Ed25519PublicKey& k)
{
    return os << util::bytes::to_hex(k.serialize());
}

std::ostream& operator<<(std::ostream& os, const Ed25519PrivateKey& k)
{
    return os << util::bytes::to_hex(k.serialize());
}

} // util namespace
} // ouinet namespace
