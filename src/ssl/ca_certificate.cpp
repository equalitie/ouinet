
#include <stdio.h>
#include <stdlib.h>
#include <iostream>

#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>

#include "ca_certificate.h"
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
// TODO: The same page says that RSA_generate_key is deprecated.
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


// Add extension using V3 code: we can set the config file as nullptr
// because we wont reference any other sections.
static void add_ext(X509 *cert, int nid, const char *value)
{
    X509_EXTENSION *ex;
    X509V3_CTX ctx;
    // This sets the 'context' of the extensions.
    // No configuration database
    X509V3_set_ctx_nodb(&ctx);
    // Issuer and subject certs: both the target since it is self signed,
    // no request and no CRL
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, (char*) value);

    if (!ex) throw runtime_error("Failed to add X509 extension");
    
    X509_add_ext(cert,ex,-1);
    X509_EXTENSION_free(ex);
}


static string read_bio(BIO* bio) {
    char* data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    return string(data, length);
};


CACertificate::CACertificate()
    : _x(X509_new())
    , _pk(EVP_PKEY_new())
{
    {
        RSA* rsa = RSA_generate_key(2048, RSA_F4, nullptr, nullptr);

        if (!EVP_PKEY_assign_RSA(_pk, rsa)) {
            throw runtime_error("Failed in EVP_PKEY_assign_RSA");
        }
    }

    X509_set_version(_x, 2);
    // TODO: Should serial be a random number?
    ASN1_INTEGER_set(X509_get_serialNumber(_x), 0);
    X509_gmtime_adj(X509_get_notBefore(_x), 0);
    // TODO: Don't hardcode the time
    X509_gmtime_adj(X509_get_notAfter(_x), (long)60*60*24*365*15 /* ~15 years */);
    X509_set_pubkey(_x, _pk);
    
    X509_NAME* name = X509_get_subject_name(_x);
    
    // This function creates and adds the entry, working out the
    // correct string type and performing checks on its length.
    // TODO: Normally we'd check the return value for errors...
    X509_NAME_add_entry_by_txt(name, "C",
            MBSTRING_ASC, (const unsigned char*) "CA", -1, -1, 0);

    X509_NAME_add_entry_by_txt(name, "CN",
            MBSTRING_ASC, (const unsigned char*) "eQualit.ie", -1, -1, 0);
    
    // Its self signed so set the issuer name to be the same as the
    // subject.
    X509_set_issuer_name(_x, name);

    // Add various standard extensions
    add_ext(_x, NID_basic_constraints, "critical,CA:TRUE");
    add_ext(_x, NID_key_usage, "critical,keyCertSign,cRLSign");
    
    add_ext(_x, NID_subject_key_identifier, "hash");
    
    // Some Netscape specific extensions
    add_ext(_x, NID_netscape_cert_type, "sslCA");
    
    if (!X509_sign(_x, _pk, EVP_sha256()))
        throw runtime_error("Failed in X509_sign");
}


string CACertificate::pem_private_key() const
{
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, _pk, nullptr, nullptr, 0, nullptr, nullptr);
    auto on_exit = defer([&] { BIO_free_all(bio); });
    return read_bio(bio);
}


string CACertificate::pem_certificate() const
{
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, _x);
    auto on_exit = defer([&] { BIO_free_all(bio); });
    return read_bio(bio);
}


string CACertificate::pem_dh_param() const
{
    return g_default_dh_param;
}


CACertificate::~CACertificate() {
    if (_x) X509_free(_x);
    if (_pk) EVP_PKEY_free(_pk);
}
