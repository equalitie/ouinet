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
class EncryptionKey;

template<typename T>
using OptBox = std::optional<rust::Box<T>>;

class Client {
public:
    // Creates a metrics client which does nothing.
    static Client noop();

    Client(const fs::path& repo_root_path, EncryptionKey encryption_key);

    void enable(util::AsioExecutor executor, AsyncCallback record_processor);
    void disable();

    bool is_enabled() const { return _is_enabled; }

    MainlineDht mainline_dht();

    Request new_origin_request();
    Request new_public_injector_request();
    Request new_private_injector_request();
    Request new_cache_request();

    std::optional<std::string> current_device_id() const;

private:
    Client() = default;

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
    void mark_success();

private:
    friend class DhtNode;

    Bootstrap(OptBox<bridge::Bootstrap> impl) : _impl(std::move(impl)) {}

    OptBox<bridge::Bootstrap> _impl;
};

// -- Requessts ------------------------------------------------------

class Request {
public:
    void finish(boost::system::error_code ec);

private:
    friend class Client;

    Request(OptBox<bridge::Request> impl): _impl(std::move(impl)) {}

    OptBox<bridge::Request> _impl;
};

// -- Encryption key  ------------------------------------------------

class EncryptionKey {
public:
    static std::optional<EncryptionKey> validate(const std::string& key_str);

    EncryptionKey(EncryptionKey&& other) : _impl(std::move(other._impl)) {}

private:
    friend class Client;

    EncryptionKey(OptBox<bridge::EncryptionKey> impl) : _impl(std::move(impl)) {}

    OptBox<bridge::EncryptionKey> _impl;
};

} // namespace
