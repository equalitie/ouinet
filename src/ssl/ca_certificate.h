#pragma once

#include <string>
#include <openssl/x509v3.h>

namespace ouinet {

class CACertificate {
public:
    CACertificate();

    std::string pem_private_key() const;
    std::string pem_certificate() const;
    std::string pem_dh_param() const;

    ~CACertificate();

private:
    X509* _x;
    EVP_PKEY* _pk;
};

} // namespace
