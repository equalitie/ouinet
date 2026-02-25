#include "util.h"

#if defined(_WIN32)

#include <wincrypt.h>

static void add_windows_root_certs(boost::asio::ssl::context &ctx) {
    HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
    if (hStore == NULL) {
        return;
    }

    X509_STORE *store = X509_STORE_new();
    PCCERT_CONTEXT pContext = NULL;
    while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != NULL) {
        X509 *x509 = d2i_X509(NULL,
                              (const unsigned char **)&pContext->pbCertEncoded,
                              pContext->cbCertEncoded);
        if(x509 != NULL) {
            X509_STORE_add_cert(store, x509);
            X509_free(x509);
        }
    }

    CertFreeCertificateContext(pContext);
    CertCloseStore(hStore, 0);

    SSL_CTX_set_cert_store(ctx.native_handle(), store);
}

#endif // _WIN32

namespace ouinet::ssl::util {

void set_default_verify_paths(asio::ssl::context& ctx) {
#ifdef _WIN32
    // Asio's `set_default_verify_paths` doesn't work on Windows unless OpenSSL
    // has been installed.
    // https://stackoverflow.com/questions/39772878/reliable-way-to-get-root-ca-certificates-on-windows
    add_windows_root_certs(ctx);
#else
    ctx.set_default_verify_paths();
#endif
}

} // namespace
