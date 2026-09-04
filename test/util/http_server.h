#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem/path.hpp>
#include <string>
#include <memory>
#include "namespaces.h"

namespace ouinet {

class Async;

class HttpServer {
private:
    struct Impl;

public:
    // The `cert_path` argument should point to a directory where certificate
    // files will be created.
    HttpServer(asio::any_io_executor, fs::path cert_dir);

    void add_resource(std::string path, std::string content);

    asio::ip::tcp::endpoint local_endpoint() const;

    std::string host() const;

    // Host and port
    std::string authority() const;

    const fs::path& certificate_path() const;

    asio::ssl::context ssl_context_for_client() const;

    HttpServer(HttpServer&&);
    ~HttpServer();

private:
    HttpServer(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> _impl;
};

} // namespace
