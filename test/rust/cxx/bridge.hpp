#pragma once

#include "rust/cxx.h"

#include <memory>
#include <boost/asio/io_context.hpp>

#include "client.h"

namespace ouinet {
namespace test {
    using Context = boost::asio::io_context;

    std::unique_ptr<Context> new_context();

    using ::ouinet::Client;

    std::unique_ptr<Client> new_client(
        Context& ctx,
        rust::Slice<const char* const> argv,
        rust::Str log_tag
    );
} // namespace test
} // namespace ouinet
