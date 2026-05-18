#pragma once

#include "rust/cxx.h"

#include <memory>
#include <boost/asio/io_context.hpp>

#include "client.h"
#include "injector.h"

namespace ouinet {
namespace test {
    struct Completer;
    struct SocketAddr;

    using Context = boost::asio::io_context;

    std::unique_ptr<Context> context_new();

    using ::ouinet::Client;

    std::unique_ptr<Client> client_new(
        Context& ctx,
        rust::Slice<const char* const> argv,
        rust::Str log_tag
    );

    void client_stop(std::unique_ptr<Client> client, rust::Box<Completer> completer);

    SocketAddr client_get_proxy_endpoint(const Client& client);

    using ::ouinet::Injector;

    std::unique_ptr<Injector> injector_new(
        Context& ctx,
        rust::Slice<const char* const> argv,
        rust::Str log_tag
    );

    void injector_stop(std::unique_ptr<Injector> injector, rust::Box<Completer> completer);

    inline rust::String injector_cache_http_public_key(const Injector& injector) {
        return injector.cache_http_public_key();
    }

    inline rust::String injector_tls_cert_file(const Injector& injector) {
        return injector.tls_cert_file().string();
    }
} // namespace test
} // namespace ouinet
