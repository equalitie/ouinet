#pragma once

#include "rust/cxx.h"

#include <memory>
#include <boost/asio/io_context.hpp>

#include "client.h"

namespace ouinet {
namespace test {
    struct Completer;
    struct SocketAddr;

    using Context = boost::asio::io_context;

    std::unique_ptr<Context> new_context();

    using ::ouinet::Client;

    std::unique_ptr<Client> new_client(
        Context& ctx,
        rust::Slice<const char* const> argv,
        rust::Str log_tag
    );

    void stop(Client& client,  rust::Box<Completer> completer);

    SocketAddr get_proxy_endpoint(const Client& client);
} // namespace test
} // namespace ouinet
