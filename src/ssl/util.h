#pragma once

#include <stdexcept>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "../generic_connection.h"
#include "../or_throw.h"


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

// Perform an SSL client handshake over the given connection `con`
// and return an SSL-tunneled connection using it as a lower layer.
//
// The verification is done for the given `host` name, using SNI.
static inline
ouinet::GenericConnection client_handshake( ouinet::GenericConnection&& con
                                          , const std::string& host
                                          , boost::asio::yield_context yield) {
    using namespace std;
    using namespace ouinet;
    namespace ssl = boost::asio::ssl;

    // SSL contexts do not seem to be reusable.
    ssl::context ssl_context{ssl::context::tls_client};
    ssl_context.set_default_verify_paths();
    ssl_context.set_verify_mode(ssl::verify_peer);
    ssl_context.set_verify_callback(ssl::rfc2818_verification(host));

    boost::system::error_code ec;

    auto ssl_sock = make_unique<ssl::stream<GenericConnection>>(move(con), ssl_context);
    // Set Server Name Indication (SNI).
    // As seen in ``http_client_async_ssl.cpp`` Boost Beast example.
    if (!::SSL_set_tlsext_host_name(ssl_sock->native_handle(), host.c_str()))
        ec = {static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
    if (!ec)
        ssl_sock->async_handshake(ssl::stream_base::client, yield[ec]);
    if (ec) return or_throw<GenericConnection>(yield, ec);

    static const auto ssl_shutter = [](ssl::stream<GenericConnection>& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericConnection(move(ssl_sock), move(ssl_shutter));
}

}}} // namespaces
