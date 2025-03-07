#pragma once

#include <string_view>
#include <boost/filesystem.hpp>
#include <boost/asio/ip/udp.hpp>
#include "rust/src/bridge.rs.h"
#include "cxx/record_processor.h"
#include "cxx/async_callback.h"

namespace ouinet::metrics {

namespace fs = boost::filesystem;
namespace asio = boost::asio;

class MainlineDht;
class DhtNode;
class Bootstrap;

class Client {
public:
    // Creates a metrics client which does nothing.
    Client();

    Client(fs::path repo_root_path);

    void set_processor(util::AsioExecutor executor, AsyncCallback record_processor);

    MainlineDht mainline_dht();

private:
    rust::Box<bridge::Client> _impl;
};

class MainlineDht {
public:
    DhtNode dht_node_ipv4();
    DhtNode dht_node_ipv6();

private:
    friend class Client;

    MainlineDht(rust::Box<bridge::MainlineDht> impl) : _impl(std::move(impl)) {}

    rust::Box<bridge::MainlineDht> _impl;
};

class DhtNode {
public:
    Bootstrap bootstrap();

private:
    friend class MainlineDht;

    DhtNode(rust::Box<bridge::DhtNode> impl) : _impl(std::move(impl)) {}

    rust::Box<bridge::DhtNode> _impl;
};

class Bootstrap {
public:
    void mark_success(asio::ip::udp::endpoint wan_endpoint);

private:
    friend class DhtNode;

    Bootstrap(rust::Box<bridge::Bootstrap> impl) : _impl(std::move(impl)) {}

    rust::Box<bridge::Bootstrap> _impl;
};

} // namespace
