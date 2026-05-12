#pragma once

#include "rust/cxx.h"

#include <memory>
#include <boost/asio/io_context.hpp>

#include "client.h"

namespace ouinet {
namespace test {
    using IoContext = boost::asio::io_context;

    std::unique_ptr<IoContext> new_io_context();

    using Client = ::ouinet::Client;

    std::unique_ptr<Client> new_client(IoContext& ctx, rust::Slice<const rust::Str> options);

} // namespace test
} // namespace ouinet
