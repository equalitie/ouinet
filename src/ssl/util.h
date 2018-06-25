#pragma once

#include <stdexcept>
#include <openssl/pem.h>

namespace ouinet { namespace ssl { namespace util {

static const long ONE_HOUR = 60*60;
static const long ONE_YEAR = 60*60*24*365;

// Add extension using V3 code: we can set the config file as nullptr
// because we wont reference any other sections.
static inline void x509_add_ext(X509 *cert, int nid, const char *value)
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

    if (!ex) throw std::runtime_error("Failed to add X509 extension");
    
    X509_add_ext(cert,ex,-1);
    X509_EXTENSION_free(ex);
}

static inline std::string read_bio(BIO* bio) {
    char* data = nullptr;
    long length = BIO_get_mem_data(bio, &data);
    return std::string(data, length);
};

}}} // namespaces
