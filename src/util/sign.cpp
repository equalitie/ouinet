#include "sign.h"
#include "../defer.h"
#include "util/bytes.h"

namespace ouinet::sign {

std::string Signature::to_hex() const {
    return util::bytes::to_hex(bytes);
}

PublicKey::PublicKey(Bytes bytes) {
    _pubkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, bytes.data(), size);
    if (!_pubkey) {
        throw std::runtime_error("failed to create public key from bytes");
    }
}

PublicKey::PublicKey(const PublicKey& other) : _pubkey(nullptr) {
    if (!other._pubkey) {
        return;
    }
    _pubkey = EVP_PKEY_dup(other._pubkey);
    if (!_pubkey) {
        throw std::runtime_error("failed to duplicate public key");
    }
}

PublicKey& PublicKey::operator=(const PublicKey& other) {
    if (_pubkey) {
        EVP_PKEY_free(_pubkey);
    }
    if (!other._pubkey) {
        _pubkey = nullptr;
        return *this;
    }
    _pubkey = EVP_PKEY_dup(other._pubkey);
    if (!_pubkey) {
        throw std::runtime_error("failed to copy public key");
    }
    return *this;
}

PublicKey::PublicKey(PublicKey&& other) : _pubkey(other._pubkey) {
    other._pubkey = nullptr;
}

PublicKey& PublicKey::operator=(PublicKey&& other) {
    if (_pubkey) {
        EVP_PKEY_free(_pubkey);
    }
    _pubkey = other._pubkey;
    other._pubkey = nullptr;
    return *this;
}

PublicKey::~PublicKey() {
    if (!_pubkey) return;
    EVP_PKEY_free(_pubkey);
}

bool PublicKey::verify(const std::string_view& message, const Signature& sig) const {
    if (!_pubkey) throw std::runtime_error("uninitialized public key");

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    auto free_mdctx_on_exit = defer([&] { EVP_MD_CTX_free(mdctx); });

    if (EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, _pubkey) <= 0) {
        throw std::runtime_error("digestVerifyInit failed");
    }

    int rc = EVP_DigestVerify(mdctx, sig.bytes.data(), sig.bytes.size(), (const unsigned char*) message.data(), message.size());

    if (rc == 1) {
        return true;
    } else if (rc == 0) {
        return false;
    } else {
        ERR_print_errors_fp(stderr);
        throw std::runtime_error("verification error");
    }
}

PublicKey::Bytes PublicKey::to_bytes() const {
    if (!_pubkey) throw std::runtime_error("secret key is not initialized");
    Bytes bytes;
    size_t len = bytes.size();
    if (EVP_PKEY_get_raw_public_key(_pubkey, bytes.data(), &len) != 1 || len != bytes.size()) {
        throw std::runtime_error("failed to convert public key to raw data");
    }
    return bytes;
}

std::string PublicKey::to_hex() const {
    return util::bytes::to_hex(to_bytes());
}

/* static */
boost::optional<PublicKey> PublicKey::from_hex(std::string_view hex)
{
    PublicKey::Bytes bytes;

    if (hex.size() != 2 * bytes.size()) {
        return boost::none;
    }

    for (size_t i = 0; i < size; ++i) {
        auto c = util::bytes::from_hex(hex[2*i], hex[2*i+1]);
        if (!c) return boost::none;
        bytes[i] = *c;
    }

    return PublicKey(bytes);
}

/* static */
SecretKey SecretKey::generate() {
    EVP_PKEY* pkey = nullptr;

    // Generate Ed25519 keypair
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!pctx) {
        throw std::runtime_error("error creating PKEY_CTX");
    }

    auto free_pctx_on_exit = defer([&] { EVP_PKEY_CTX_free(pctx); });

    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        throw std::runtime_error("key generation failed");
    }

    return SecretKey(pkey);
}

SecretKey::SecretKey(Bytes bytes) {
    _pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, bytes.data(), size);
    if (!_pkey) {
        throw std::runtime_error("failed to create private key from bytes");
    }
}

SecretKey::SecretKey(const SecretKey& other) : _pkey(nullptr) {
    if (!other._pkey) {
        return;
    }
    _pkey = EVP_PKEY_dup(other._pkey);
    if (!_pkey) {
        throw std::runtime_error("failed to copy construct secret key");
    }
}

SecretKey::SecretKey(SecretKey&& other) : _pkey(other._pkey) {
    other._pkey = nullptr;
}

SecretKey& SecretKey::operator=(SecretKey&& other) {
    if (_pkey) {
        EVP_PKEY_free(_pkey);
    }
    _pkey = other._pkey;
    other._pkey = nullptr;
    return *this;
}

SecretKey::~SecretKey() {
    if (!_pkey) return;
    EVP_PKEY_free(_pkey);
}

PublicKey SecretKey::public_key() const {
    PublicKey::Bytes bytes;
    size_t len = bytes.size();
    if (EVP_PKEY_get_raw_public_key(_pkey, bytes.data(), &len) != 1) {
        throw std::runtime_error("failed to get public key from secret key");
    }
    return PublicKey(bytes);
}

Signature SecretKey::sign(boost::string_view message) const {
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();

    if (!mdctx) {
        throw std::runtime_error("error creating MD_CTX");
    }

    auto free_mdctx_on_exit = defer([&] { EVP_MD_CTX_free(mdctx); });

    if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, _pkey) <= 0) {
        throw std::runtime_error("digestSignInit failed");
    }

    /* Determine signature length */
    size_t sig_len;
    if (EVP_DigestSign(mdctx, NULL, &sig_len, (const unsigned char*) message.data(), message.size()) <= 0) {
        throw std::runtime_error("digestSign (size) failed");
    }

    if (sig_len != Signature::size) {
        throw std::runtime_error("invalid signature size");
    }

    Signature sig;

    if (EVP_DigestSign(mdctx, sig.bytes.data(), &sig_len, (const unsigned char*) message.data(), message.size()) <= 0) {
        throw std::runtime_error("digestSign failed");
    }

    return sig;
}

SecretKey::Bytes SecretKey::to_bytes() const {
    if (!_pkey) throw std::runtime_error("secret key is not initialized");
    Bytes bytes;
    size_t len = bytes.size();
    if (EVP_PKEY_get_raw_private_key(_pkey, bytes.data(), &len) != 1 || len != bytes.size()) {
        throw std::runtime_error("failed to convert private key to raw data");
    }
    return bytes;
}

std::string SecretKey::to_hex() const {
    return util::bytes::to_hex(to_bytes());
}

/* static */
boost::optional<SecretKey> SecretKey::from_hex(boost::string_view hex)
{
    SecretKey::Bytes bytes;

    if (hex.size() != 2 * bytes.size()) {
        return boost::none;
    }

    for (size_t i = 0; i < bytes.size(); ++i) {
        auto c = util::bytes::from_hex(hex[2*i], hex[2*i+1]);
        if (!c) return boost::none;
        bytes[i] = *c;
    }

    return SecretKey(bytes);
}

std::istream& operator>>(std::istream& os, SecretKey& sk)
{
    auto i   = std::istreambuf_iterator<char>(os);
    auto end = std::istreambuf_iterator<char>();

    SecretKey::Bytes bytes;

    size_t j = 0;

    while (true) {
        if (i == end) break;
        auto h1 = *i++;
        if (i == end) break;
        auto h2 = *i++;
        auto c = util::bytes::from_hex(h1, h2);
        if (!c) throw std::runtime_error("failed to parse secret key (not hex)");
        bytes[j++] = *c;
        if (j > bytes.size()) throw std::runtime_error("failed to parse secret key (long)");
    }

    if (j != bytes.size()) throw std::runtime_error("failed to parse secret key (short)");

    sk = SecretKey(bytes);

    return os;
}

} // namespaces
