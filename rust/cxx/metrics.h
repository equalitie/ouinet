#pragma once

#include <string_view>
#include <boost/filesystem.hpp>
#include <boost/asio/ip/udp.hpp>
#include "ouinet-rs/src/bridge.rs.h"
#include "cxx/record_processor.h"
#include "cxx/async_callback.h"

namespace ouinet::metrics {

namespace fs = boost::filesystem;
namespace asio = boost::asio;

class MainlineDht;
class DhtNode;
class Bootstrap;
class Request;

template<typename T>
using OptBox = std::optional<rust::Box<T>>; 

class Client {
public:
    // Creates a metrics client which does nothing.
    Client();

    Client(fs::path repo_root_path);

    void enable(util::AsioExecutor executor, AsyncCallback record_processor);
    void disable();

    bool is_enabled() const { return _is_enabled; }

    MainlineDht mainline_dht();

    Request new_origin_request();
    Request new_injector_request();
    Request new_cache_request();

private:
    OptBox<bridge::Client> _impl;
    bool _is_enabled = false;
};

// -- DHT ------------------------------------------------------------

class MainlineDht {
public:
    DhtNode dht_node_ipv4();
    DhtNode dht_node_ipv6();

private:
    friend class Client;

    MainlineDht(OptBox<bridge::MainlineDht> impl) : _impl(std::move(impl)) {}

    OptBox<bridge::MainlineDht> _impl;
};

class DhtNode {
public:
    Bootstrap bootstrap();

private:
    friend class MainlineDht;

    DhtNode(OptBox<bridge::DhtNode> impl) : _impl(std::move(impl)) {}

    OptBox<bridge::DhtNode> _impl;
};

class Bootstrap {
public:
    void mark_success(asio::ip::udp::endpoint wan_endpoint);

private:
    friend class DhtNode;

    Bootstrap(OptBox<bridge::Bootstrap> impl) : _impl(std::move(impl)) {}

    OptBox<bridge::Bootstrap> _impl;
};

// -- Requessts ------------------------------------------------------

class Request {
public:
    void start();
    void success();
    void failure();

private:
    friend class Client;

    Request(OptBox<bridge::Request> impl): _impl(std::move(impl)) {};

    OptBox<bridge::Request> _impl;
};

} // namespace
