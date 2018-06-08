#pragma once

#include <string>
#include <openssl/x509v3.h>

namespace ouinet {

class CACertificate {
public:
    CACertificate();
    CACertificate(std::string pem_cert, std::string pem_key, std::string pem_dh);

    const std::string& pem_private_key() const { return _pem_private_key; }
    const std::string& pem_certificate() const { return _pem_certificate; }
    const std::string& pem_dh_param()    const { return _pem_dh_param;    }

    ~CACertificate();

private:
    friend class DummyCertificate;

    X509_NAME* get_subject_name() const;
    EVP_PKEY*  get_private_key() const;

    unsigned long next_serial_number() {
        return _next_serial_number++;
    }

private:
    X509* _x;
    EVP_PKEY* _pk;

    std::string _pem_private_key;
    std::string _pem_certificate;
    std::string _pem_dh_param;

    unsigned long _next_serial_number = 0;
};

} // namespace
