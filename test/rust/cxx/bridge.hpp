#pragma once

#include "rust/cxx.h"

#include <memory>
#include <boost/asio/io_context.hpp>

#include "client.h"

namespace ouinet {
namespace test {
    struct SocketAddr;

    using Context = boost::asio::io_context;

    std::unique_ptr<Context> new_context();

    using ::ouinet::Client;

    std::unique_ptr<Client> new_client(
        Context& ctx,
        rust::Vec<rust::String> argv,
        rust::Str log_tag
    );

    SocketAddr get_proxy_endpoint_raw(const Client& client);
} // namespace test
} // namespace ouinet
