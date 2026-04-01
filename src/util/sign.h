#pragma once

#include <openssl/evp.h>
#include <openssl/err.h>
#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>
#include <ostream>
#include <istream>

namespace ouinet::sign {

struct Signature {
    static const size_t size = 64;
    using Bytes = std::array<uint8_t, size>;
    Bytes bytes;

    std::string to_hex() const;

    friend std::ostream& operator<<(std::ostream& os, Signature const& sig) {
        return os << sig.to_hex();
    }
};

class PublicKey {
public:
    static const size_t size = 32;
    using Bytes = std::array<uint8_t, size>;

    PublicKey() {}

    PublicKey(Bytes bytes);

    PublicKey(PublicKey const&);
    PublicKey& operator=(PublicKey const&);

    PublicKey(PublicKey&& other);
    PublicKey& operator=(PublicKey&& other);

    bool verify(const std::string_view& message, const Signature& sig) const;

    Bytes to_bytes() const;

    std::string to_hex() const;

    static
    boost::optional<PublicKey> from_hex(std::string_view hex);

    friend std::ostream& operator<<(std::ostream& os, PublicKey const& pk) {
        return os << pk.to_hex();
    }

    ~PublicKey();

private:
    EVP_PKEY* _pubkey = nullptr;
};

class SecretKey {
public:
    static const size_t size = 32;
    using Bytes = std::array<uint8_t, size>;

    static SecretKey generate();

    SecretKey() {}

    SecretKey(Bytes bytes);

    SecretKey(SecretKey const&);
    SecretKey& operator=(SecretKey const&) = delete;

    SecretKey(SecretKey&& other);

    SecretKey& operator=(SecretKey&& other);

    PublicKey public_key() const;

    Signature sign(boost::string_view message) const;

    Bytes to_bytes() const;

    std::string to_hex() const;

    static boost::optional<SecretKey> from_hex(boost::string_view hex);

    friend std::ostream& operator<<(std::ostream& os, SecretKey const& sk) {
        return os << sk.to_hex();
    }

    friend std::istream& operator>>(std::istream&, SecretKey&);

    ~SecretKey();

private:
    SecretKey(EVP_PKEY* pkey) : _pkey(pkey) {}

private:
    EVP_PKEY* _pkey = nullptr;
};

} // namespaces
