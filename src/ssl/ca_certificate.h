#pragma once

#include <string>
#include <openssl/x509v3.h>

namespace ouinet {

// TODO: Properly split CA and end certificate machinery and interface
// into separate classes (and then rename this file).

class BaseCertificate {
protected:
    BaseCertificate(const std::string& cn, bool is_ca);

public:
    BaseCertificate(std::string pem_cert, std::string pem_key, std::string pem_dh);

    const std::string& pem_private_key() const { return _pem_private_key; }
    const std::string& pem_certificate() const { return _pem_certificate; }
    const std::string& pem_dh_param()    const { return _pem_dh_param;    }

    ~BaseCertificate();

    // Which is version 3 according to
    // <https://www.openssl.org/docs/man1.1.0/crypto/X509_set_version.html>.
    static const long x509_version = 2;

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

    unsigned long _next_serial_number;
};

class CACertificate : public BaseCertificate {
public:
    CACertificate(const std::string& cn)
        : BaseCertificate(cn, true) {};
    CACertificate(std::string pem_cert, std::string pem_key, std::string pem_dh)
        : BaseCertificate(pem_cert, pem_key, pem_dh) {};
    ~CACertificate() {};
};

class EndCertificate : public BaseCertificate {
public:
    EndCertificate(const std::string& cn)
        : BaseCertificate(cn, false) {};
    EndCertificate(std::string pem_cert, std::string pem_key, std::string pem_dh)
        : BaseCertificate(pem_cert, pem_key, pem_dh) {};
    ~EndCertificate() {};
};

} // namespace
