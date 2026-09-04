#include "http_server.h"
#include "util/async.h"
#include "ssl/ca_certificate.h"
#include "ssl/util.h"
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <optional>
#include <iostream>
#include <sstream>
#include <map>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

namespace ouinet {

static const char* HOST_NAME = "localhost";

using tcp = asio::ip::tcp;

class Exception : public std::exception {
public:
    Exception(std::string msg, sys::error_code ec) : _msg(msg + "(" + ec.message() + ")") {}

    virtual const char* what() const noexcept override {
        auto& full_msg = const_cast<std::optional<std::string>&>(_full_msg);
        if (!full_msg) {
            full_msg = "Exception from HttpServer: " + _msg;
        }
        return full_msg->c_str();
    }

    virtual ~Exception() noexcept override {}

private:
    std::string _msg;
    std::optional<std::string> _full_msg;
};

template<class F> void spawn(asio::any_io_executor ex, Cancel cancel, F f) {
    asio::spawn(ex, [cancel, f = std::move(f)] (asio::yield_context yield) mutable {
        f(Async(yield, cancel));
    },
    [] (std::exception_ptr ep) {
        try {
            if (ep) std::rethrow_exception(ep);
        }
        catch (Async::Cancelled const&) {}
        catch (std::exception const& e) {
            std::cerr << "HttpServer loop: " << e.what() << "\n";
            std::terminate();
        }
    });
}

// This helped when writing this class (also read the comments).
// https://stackoverflow.com/a/8407739/273348
struct Ssl {
    fs::path key_path, cert_path;
    asio::ssl::context ctx{asio::ssl::context::tls_server};

    Ssl(fs::path cert_dir):
        key_path(cert_dir / "server.key"),
        cert_path(cert_dir / "server.crt")
    {
        EVP_PKEY *server_key = nullptr;
        X509 *server_cert = nullptr;

        try {
            server_key = generate_rsa_key(2048);
            server_cert = create_cert(server_key, HOST_NAME);

            write_key(server_key, key_path);
            write_cert(server_cert, cert_path);

            EVP_PKEY_free(server_key);
            X509_free(server_cert);
        }
        catch (const std::exception& e) {
            // Cleanup
            if (server_key) EVP_PKEY_free(server_key);
            if (server_cert) X509_free(server_cert);
            throw;
        }

        ctx.use_certificate_chain_file(cert_path.string());
        ctx.use_private_key_file(key_path.string(), asio::ssl::context::pem);

    }

    static X509* create_cert(EVP_PKEY* key, const std::string& cn)
    {
        X509* cert = X509_new();
    
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    
        X509_gmtime_adj(X509_get_notBefore(cert), 0);
        X509_gmtime_adj(X509_get_notAfter(cert), 31536000L * 10);
    
        X509_set_pubkey(cert, key);
    
        // Subject
        X509_NAME* name = X509_NAME_new();
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                  (unsigned char*)cn.c_str(), -1, -1, 0);
        X509_set_subject_name(cert, name);
    
        // Issuer
        X509_set_issuer_name(cert, name); // self-signed CA
    
        // Extensions
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
    
        X509V3_set_ctx(&ctx, nullptr, cert, nullptr, nullptr, 0);
    
        X509_EXTENSION* ex = X509V3_EXT_conf_nid(
            nullptr, &ctx,
            NID_basic_constraints,
            (char*)"CA:FALSE");
    
        X509_add_ext(cert, ex, -1);
        X509_EXTENSION_free(ex);
    
        std::stringstream ss;
        ss << "DNS:" << cn << ",IP:127.0.0.1";

        ex = X509V3_EXT_conf_nid(
            nullptr, &ctx,
            NID_subject_alt_name,
            (char*)ss.str().c_str());
    
        X509_add_ext(cert, ex, -1);
        X509_EXTENSION_free(ex);
    
        // Sign certificate
        X509_sign(cert, key, EVP_sha256());
    
        X509_NAME_free(name);
        return cert;
    }

    static EVP_PKEY* generate_rsa_key(int bits)
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!ctx) throw std::runtime_error("CTX failed");
    
        if (EVP_PKEY_keygen_init(ctx) <= 0)
            throw std::runtime_error("keygen_init failed");
    
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0)
            throw std::runtime_error("set bits failed");
    
        EVP_PKEY* key = nullptr;
        if (EVP_PKEY_keygen(ctx, &key) <= 0)
            throw std::runtime_error("keygen failed");
    
        EVP_PKEY_CTX_free(ctx);
        return key;
    }


    static void write_key(EVP_PKEY* key, fs::path const& path)
    {
        FILE* f = fopen(path.string().c_str(), "wb");
        if (!f) throw std::runtime_error("file open failed");
    
        PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    
        fclose(f);
    }


    static void write_cert(X509* cert, fs::path const& path)
    {
        FILE* f = fopen(path.string().c_str(), "wb");
        if (!f) throw std::runtime_error("file open failed");
    
        PEM_write_X509(f, cert);
    
        fclose(f);
    }
};

struct HttpServer::Impl {
    tcp::acceptor acceptor;
    Ssl ssl;
    std::map<std::string, http::response<http::string_body>> resources;
    Cancel cancel;

    ~Impl() {
        cancel();
    }

    Impl(tcp::acceptor acceptor, Ssl ssl)
        : acceptor(std::move(acceptor))
        , ssl(std::move(ssl))
    {
        spawn(acceptor.get_executor(), cancel, [&] (Async yield) {
            run_accept(yield);
        });
    }

    void run_accept(Async yield) {
        auto slot = yield.cancel_slot([&] { if (acceptor.is_open()) acceptor.close(); });

        while (true) {
            tcp::socket socket(yield.get_executor());
            auto r = acceptor.async_accept(socket, yield);
            if (!r) throw Exception("accept", r.error());

            yield.spawn([&, socket = std::move(socket)] (Async yield) mutable {
                asio::ssl::stream<beast::tcp_stream> stream(std::move(socket), ssl.ctx);

                auto slot = yield.cancel_slot([&] {
                    auto& socket = stream.lowest_layer();
                    if (socket.is_open()) socket.close();
                });

                run_session(stream, yield);
            });
        }
    }

    void run_session(asio::ssl::stream<beast::tcp_stream>& stream, Async yield) {
        auto r = stream.async_handshake(asio::ssl::stream_base::server, yield);
        if (!r) throw Exception("handshake", r.error());

        beast::flat_buffer buffer;

        while (true) {
            http::request<http::string_body> req;
            auto read_r = http::async_read(stream, buffer, req, yield);
            if (!read_r.has_value()) {
                if (read_r.error() == http::error::end_of_stream) {
                    break;
                }
                // https://github.com/boostorg/beast/issues/824
                if (read_r.error() == asio::ssl::error::stream_truncated) {
                    return;
                }
                throw Exception("read", read_r.error());
            }

            http::message_generator res = handle_request(std::move(req));
            bool keep_alive = res.keep_alive();
            auto write_r = beast::async_write(stream, std::move(res), yield);

            if(!write_r.has_value()) throw Exception("write", write_r.error());

            if(!keep_alive) break;
        }

        r = stream.async_shutdown(yield);
        if(!r) throw Exception("shutdown", r.error());
    }

    http::message_generator handle_request(http::request<http::string_body> req) {
        auto target = req.target();

        const std::string absolute_form_prefix = "https://" + authority();

        // https://datatracker.ietf.org/doc/html/rfc9112#section-3.2
        if (target.starts_with(absolute_form_prefix)) {
            target.remove_prefix(absolute_form_prefix.size());
        }

        auto ri = resources.find(target);

        if (ri == resources.end()) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::server, "Ouinet test server");
            res.set(http::field::content_type, "text/html");
            res.body() = util::str("Target \"", req.target(), "\" not found.\n");
            res.prepare_payload();
            return res;
        }

        auto res = ri->second;

        res.version(req.version());
        res.keep_alive(req.keep_alive());

        return res;
    }

    std::string authority() const {
        std::stringstream ss;
        ss << HOST_NAME << ":" << acceptor.local_endpoint().port();
        return ss.str();
    }
};

HttpServer::HttpServer(asio::any_io_executor exec, fs::path cert_dir) {
    sys::error_code ec;

    tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    tcp::acceptor acceptor(exec);
    acceptor.open(ep.protocol(), ec);
    if(ec) throw Exception("open", ec);

    acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if(ec) throw Exception("set_option", ec);

    acceptor.bind(ep, ec);
    if(ec) throw Exception("bind", ec);

    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if(ec) throw Exception("listen", ec);

    _impl = std::make_unique<Impl>(std::move(acceptor), Ssl(cert_dir));
}

asio::ip::tcp::endpoint HttpServer::local_endpoint() const {
    return _impl->acceptor.local_endpoint();
}

const fs::path& HttpServer::certificate_path() const {
    return _impl->ssl.cert_path;
}

std::string HttpServer::host() const {
    return HOST_NAME;
}

std::string HttpServer::authority() const {
    return _impl->authority();
}

void HttpServer::add_resource(std::string path, std::string body) {
    http::response<http::string_body> rs{http::status::ok, 11};
    rs.set(http::field::server, "Ouinet test server");
    rs.set(http::field::content_type, "text/html");
    rs.body() = std::move(body);
    rs.prepare_payload();

    _impl->resources.insert(std::pair(std::move(path), std::move(rs)));
}

HttpServer::HttpServer(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
HttpServer::HttpServer(HttpServer&&) = default;
HttpServer::~HttpServer() = default;

asio::ssl::context HttpServer::ssl_context_for_client() const {
    asio::ssl::context ctx{asio::ssl::context::tls_client};

    ctx.load_verify_file(certificate_path().string());
    ctx.set_verify_mode(asio::ssl::verify_peer);

    return ctx;
}

} // namespace
