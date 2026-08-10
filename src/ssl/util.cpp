#include "util.h"

#ifdef OUINET_HAS_HARDCODED_CA_CERTS
#   include "cacert.pem.h"
#elif defined(_WIN32)
#   include <wincrypt.h>

    //
    // If you're running on a fresh Windows (e.g. Windows docker
    // container), use these to install CA certs:
    //
    //     # Download latest SST from Microsoft
    //     CertUtil –generateSSTFromWU RootStore.sst
    //     # Import RootStore.sst to Trusted Root CA Store
    //     $file=Get-ChildItem -Path Rootstore.sst
    //     $file | Import-Certificate -CertStoreLocation 'Cert:\LocalMachine\Root\'
    // 
    // Also see
    //
    //     https://stackoverflow.com/questions/39772878/reliable-way-to-get-root-ca-certificates-on-windows
    //
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

void load_tls_ca_certificates(asio::ssl::context& ctx) {
#ifdef OUINET_HAS_HARDCODED_CA_CERTS
        ctx.add_certificate_authority(
            asio::const_buffer(
                hardcoded_ca_certificates.data(),
                hardcoded_ca_certificates.size()
            ));
#elif _WIN32
        add_windows_root_certs(ctx);
#else
        ctx.set_default_verify_paths();
#endif
}

void load_tls_ca_certificates(asio::ssl::context& ctx, const std::string& path_str) {
    if (path_str.empty()) {
        load_tls_ca_certificates(ctx);
        return;
    }

    fs::path path = path_str;

    if (!exists(path)) {
        std::ostringstream ss;
        ss << "Can not read CA certificates from \"" << path << "\": "
           << "No such file or directory";
        throw std::runtime_error(ss.str());
    }

    if (fs::is_directory(path)) {
        ctx.add_verify_path(path_str);
        return;
    }

    std::ostringstream ss;
    ss << boost::nowide::ifstream(path).rdbuf();
    ctx.add_certificate_authority(asio::buffer(ss.str()));
}

} // namespace ouinet::ssl::util
