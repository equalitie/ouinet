#pragma once

#include <string_view>
#include <boost/filesystem.hpp>
#include <boost/asio/ip/udp.hpp>
#include "ouinet-rs/src/metrics/bridge.rs.h"
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

enum class SetAuxResult {
    Ok = 0,
    BadRecordId,
    Noop,
};

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
    Request new_cache_in_request();
    Request new_cache_out_request();

    // Meter number of bytes transferred when this node acts as a bridge
    // between the injector and the other client.
    void bridge_transfer_i2c(size_t);
    void bridge_transfer_c2i(size_t);

    // Returns `false` if this is a `noop` client.
    SetAuxResult set_aux_key_value(std::string_view record_id, std::string_view key, std::string_view value);

    std::optional<std::string> current_device_id() const;
    std::optional<std::string> current_record_id() const;

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

// -- Requests ------------------------------------------------------

class Request {
public:
    // Mark how much data was transferred in the body of the response.  Can be
    // called repeatedly (e.g. because the body is uses chunked encoding).
    void increment_transfer_size(size_t);

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

private:
    friend class Client;

    EncryptionKey(OptBox<bridge::EncryptionKey> impl) : _impl(std::move(impl)) {}

    OptBox<bridge::EncryptionKey> _impl;
};

} // namespace
