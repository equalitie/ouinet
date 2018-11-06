#pragma once

#include <stdexcept>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "../generic_stream.h"
#include "../or_throw.h"
#include "../util/signal.h"


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

// Perform an SSL client handshake over the given stream `con`
// and return an SSL-tunneled stream using it as a lower layer.
//
// The verification is done for the given `host` name (if non-empty),
// using SNI.  Verification against a valid CA is done in any case.
template<class Stream>
static inline
ouinet::GenericStream
client_handshake( Stream&& con
                , boost::asio::ssl::context& ssl_context
                , const std::string& host
                , Signal<void()>& abort_signal
                , boost::asio::yield_context yield)
{
    using namespace std;
    using namespace ouinet;
    namespace ssl = boost::asio::ssl;

    boost::system::error_code ec;

    auto ssl_sock = make_unique<ssl::stream<Stream>>(move(con), ssl_context);
    bool check_host = host.length() > 0;
    if (check_host)
        ssl_sock->set_verify_callback(ssl::rfc2818_verification(host));

    // Set Server Name Indication (SNI).
    // As seen in ``http_client_async_ssl.cpp`` Boost Beast example.
    if (check_host && !::SSL_set_tlsext_host_name(ssl_sock->native_handle(), host.c_str()))
        ec = {static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};

    if (!ec) {
        auto slot = abort_signal.connect([&] { ssl_sock->next_layer().close(); });
        ssl_sock->async_handshake(ssl::stream_base::client, yield[ec]);
    }

    if (ec) return or_throw<GenericStream>(yield, ec);

    static const auto ssl_shutter = [](ssl::stream<Stream>& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericStream(move(ssl_sock), move(ssl_shutter));
}

static inline
boost::asio::ssl::context
get_server_context( const std::string& cert_chain
                  , const std::string& private_key
                  , const std::string& dh)
{
    namespace ssl = boost::asio::ssl;
    ssl::context ssl_context{ssl::context::tls_server};

    ssl_context.set_options( ssl::context::default_workarounds
                           | ssl::context::no_sslv2
                           | ssl::context::single_dh_use);

    ssl_context.use_certificate_chain(
            asio::buffer(cert_chain.data(), cert_chain.size()));

    ssl_context.use_private_key( asio::buffer( private_key.data()
                                             , private_key.size())
                               , ssl::context::file_format::pem);

    ssl_context.use_tmp_dh(asio::buffer(dh.data(), dh.size()));

    ssl_context.set_password_callback(
        [](std::size_t, ssl::context_base::password_purpose)
        {
            assert(0 && "TODO: Not yet supported");
            return "";
        });

    return ssl_context;
}

}}} // namespaces
