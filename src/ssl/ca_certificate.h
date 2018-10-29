#pragma once

#include <iostream>
#include <sstream>
#include <string>

#include <boost/filesystem.hpp>
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

inline void _report_load(const CACertificate&) {
    std::cout << "Loading existing CA certificate..." << std::endl;
}
inline void _report_generate(const CACertificate&) {
    std::cout << "Generating and storing CA certificate..." << std::endl;
}

inline void _report_load(const EndCertificate&) {
    std::cout << "Loading existing TLS end certificate..." << std::endl;
}
inline void _report_generate(const EndCertificate&) {
    std::cout << "Generating and storing TLS end certificate..." << std::endl;
}

// Load a TLS certificate of the given class `Cert`
// from the PEM files for certificate, key and Diffie-Hellman parameters
// at the given paths.
// If the files are missing,
// generate a self-signed certificate with the given common name `cn`,
// store its parts in the given paths and return it.
template<class Cert>
inline
std::unique_ptr<Cert>
get_or_gen_tls_cert( const std::string& cn
                   , const boost::filesystem::path& tls_cert_path
                   , const boost::filesystem::path& tls_key_path
                   , const boost::filesystem::path& tls_dh_path )
{
    namespace fs = boost::filesystem;
    std::unique_ptr<Cert> tls_certificate;

    if (fs::exists(tls_cert_path) && fs::exists(tls_key_path) && fs::exists(tls_dh_path)) {
        _report_load(*tls_certificate);
        auto read_pem = [](auto path) {
            std::stringstream ss;
            ss << fs::ifstream(path).rdbuf();
            return ss.str();
        };
        auto cert = read_pem(tls_cert_path);
        auto key = read_pem(tls_key_path);
        auto dh = read_pem(tls_dh_path);
        tls_certificate = std::make_unique<Cert>(cert, key, dh);
    } else {
        _report_generate(*tls_certificate);
        tls_certificate = std::make_unique<Cert>(cn);
        fs::ofstream(tls_cert_path) << tls_certificate->pem_certificate();
        fs::ofstream(tls_key_path) << tls_certificate->pem_private_key();
        fs::ofstream(tls_dh_path) << tls_certificate->pem_dh_param();
    }

    return tls_certificate;
}

} // namespace
