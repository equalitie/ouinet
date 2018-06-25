#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <ctime>

#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>

#include "ca_certificate.h"
#include "util.h"
#include "../defer.h"
#include "../util.h"

using namespace std;
using namespace ouinet;

// Inspired by:
//
//     https://opensource.apple.com/source/OpenSSL/OpenSSL-22/openssl/demos/x509/mkcert.c
//
// Changes made:
//
//     * Changed the `num` parameter to RSA_generate_key from 512 to 2048
//     * Use RSA_generate_key_ex instead of deprecated RSA_generate_key_
//       with hints from <https://stackoverflow.com/a/16393292>
//     * Replaced EVP_md5 for EVP_sha256 in X509_sign


// TODO: This page:
//
//     https://www.openssl.org/docs/man1.1.0/crypto/RSA_generate_key.html
//
// says that:
//
//     "The pseudo-random number generator must be seeded prior to calling
//      RSA_generate_key_ex()"
//
// To do it:
//
//     https://stackoverflow.com/a/12094032/273348
//


// Generated with:
//     openssl dhparam -out dhparam.pem 2048
static string g_default_dh_param =
    "-----BEGIN DH PARAMETERS-----\n"
    "MIIBCAKCAQEAmMfLh4XcQ2ZHEIuYwydRBtEAxqAwHBavSAuDYiBzQhx34VWop3Lh\n"
    "vb0dC5ALrSH40GVHAqzK3B1R2KW22Y0okgbEYkhQfezHSIA+JVF34iI68TIDUYmo\n"
    "ug66gnaNYoqH+6vatR8ZScIjTCPHPqUby527nq0PG0Vm050ArE0Pc5KXypFcYVae\n"
    "K6vWsjCIgUVImVNgrILPT5gUAr0xDdRwR9ALvINPhu4W9Hs0/QdMoevS/zkq/ZZv\n"
    "H2kesQbEjvVeMAcSTpsrKJfKubAH+qWbOZX+WMuFzZh4MoX8ZAhMS+9mP8O3DXgn\n"
    "axuZUTw+rQsopobaGu/taeO9ntqLATPZEwIBAg==\n"
    "-----END DH PARAMETERS-----\n";

// This is just to reduce the chances that
// running the client, stopping it and running it again
// produces certificates with the same serial number.
// (One would need to create
// more than CERT_SERNUM_SCALE per second in a sustained fashion
// then restart the client immediately to get a clash.)
static const int CERT_SERNUM_SCALE = 1000;


CACertificate::CACertificate()
    : _x(X509_new())
    , _pk(EVP_PKEY_new())
    , _next_serial_number(std::time(nullptr) * CERT_SERNUM_SCALE)
{
    {
        RSA* rsa = RSA_new();
        if (!rsa) {
            throw runtime_error("Failed to allocate new RSA key");
        }

        BIGNUM* exp = BN_new();
        if (!exp) {
            RSA_free(rsa);
            throw runtime_error("Failed to allocate exponent");
        }
        // Exponent and modulus size below as recommended by
        // RSA_generate_key documentation.
        BN_set_word(exp, 65537);

        if (!RSA_generate_key_ex(rsa, 2048, exp, nullptr)) {
            BN_free(exp);
            RSA_free(rsa);
            throw runtime_error("Failed to generate new RSA key");
        }

        BN_free(exp);  // no longer used

        if (!EVP_PKEY_assign_RSA(_pk, rsa)) {
            RSA_free(rsa);
            throw runtime_error("Failed in EVP_PKEY_assign_RSA");
        }
    }

    X509_set_version(_x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(_x), next_serial_number());
    // Avoid signature issues because of time zone differences.
    // See [Mitmproxy can't record traffic when time set with 1 hour ago.](https://github.com/mitmproxy/mitmproxy/issues/200).
    X509_gmtime_adj(X509_get_notBefore(_x), -48*ssl::util::ONE_HOUR);
    // A long-enough expiration for the CA (sort of everlasting).
    // Please note that certificates emitted by this CA may not be more than 39 months old:
    // [Validity Period, 9.4.1](https://cabforum.org/wp-content/uploads/BRv1.2.3.pdf).
    X509_gmtime_adj(X509_get_notAfter(_x), 15*ssl::util::ONE_YEAR);
    X509_set_pubkey(_x, _pk);
    
    X509_NAME* name = X509_get_subject_name(_x);
    
    // This function creates and adds the entry, working out the
    // correct string type and performing checks on its length.
    // TODO: Normally we'd check the return value for errors...
    X509_NAME_add_entry_by_txt(name, "CN",
            MBSTRING_ASC, (const unsigned char*) "Your own local Ouinet client", -1, -1, 0);
    
    // Its self signed so set the issuer name to be the same as the
    // subject.
    X509_set_issuer_name(_x, name);

    // Add various standard extensions
    ssl::util::x509_add_ext(_x, NID_basic_constraints, "critical,CA:TRUE");
    ssl::util::x509_add_ext(_x, NID_key_usage, "critical,keyCertSign,cRLSign");
    ssl::util::x509_add_ext(_x, NID_subject_key_identifier, "hash");
    
    // Some Netscape specific extensions
    ssl::util::x509_add_ext(_x, NID_netscape_cert_type, "sslCA");
    
    if (!X509_sign(_x, _pk, EVP_sha256()))
        throw runtime_error("Failed in X509_sign");

    {
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, _pk, nullptr, nullptr, 0, nullptr, nullptr);
        _pem_private_key = ssl::util::read_bio(bio);
        BIO_free_all(bio);
    }

    {
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(bio, _x);
        _pem_certificate = ssl::util::read_bio(bio);
        BIO_free_all(bio);
    }

    _pem_dh_param = g_default_dh_param;
}

CACertificate::CACertificate(std::string pem_cert, std::string pem_key, std::string pem_dh)
    : _pem_private_key(move(pem_key))
    , _pem_certificate(move(pem_cert))
    , _pem_dh_param(move(pem_dh))
    , _next_serial_number(std::time(nullptr) * CERT_SERNUM_SCALE)
{
    {
        BIO* bio = BIO_new_mem_buf(_pem_private_key.data(), _pem_private_key.size());
        EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free_all(bio);
        if (!key)
            throw runtime_error("Failed to parse CA PEM key");
        _pk = key;
    }
    {
        BIO* bio = BIO_new_mem_buf(_pem_certificate.data(), _pem_certificate.size());
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free_all(bio);
        if (!cert)
            throw runtime_error("Failed to parse CA PEM certificate");
        _x = cert;
    }
    {
        BIO* bio = BIO_new_mem_buf(_pem_dh_param.data(), _pem_dh_param.size());
        DH* dh = PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr);
        BIO_free_all(bio);
        if (!dh)
            throw runtime_error("Failed to parse CA PEM DH parameters");
        DH_free(dh);  // just to check it is correct
    }
}


X509_NAME* CACertificate::get_subject_name() const
{
    return X509_get_subject_name(_x);
}


EVP_PKEY* CACertificate::get_private_key() const
{
    return _pk;
}


CACertificate::~CACertificate() {
    if (_x) X509_free(_x);
    if (_pk) EVP_PKEY_free(_pk);
}
