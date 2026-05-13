#pragma once

#include "ouiservice.h"

#include "../address.h"
#include "tunnel.h"

namespace i2p::client {
    class I2PClientTunnel;
    class ClientDestination;
}

namespace ouinet {

class Async;

namespace i2p_direct {

class Service;

class Client : public ouinet::OuiServiceImplementationClient {
    using executor_type = asio::any_io_executor;

public:
    Client( std::shared_ptr<Service> service
          , const I2pAddress& target_id
          , uint32_t timeout
          , const executor_type&
          , std::shared_ptr<i2p::client::ClientDestination> destination = nullptr);

    ~Client();

    executor_type get_executor() { return _exec; }

    [[nodiscard]]
    sys::error_code start(Async) override;

    void stop() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code>
    connect(Async) override;

    // Returns the target I2P address this client connects to
    const I2pAddress& get_target_id() const { return _target_id; }

    // Connect without the ouinet i2p handshake protocol. Use this for test  or for communicating
    // with non-ouinet I2P hosts such as BEP3 trackers.
    [[nodiscard]]
    std::expected<GenericStream, sys::error_code>
    connect_without_handshake(Async);

private:
    std::shared_ptr<Service> _service;
    executor_type _exec;
    I2pAddress _target_id;
    uint32_t _timeout;

    // The destinaton where client requests originate from
    std::shared_ptr<i2p::client::ClientDestination> _destination;

    std::unique_ptr<Tunnel> _client_tunnel; //the tunnel is a pointer because
    //the client can be stopped (tunnel gets destroyed) and started again
    uint16_t _port;
    // Triggered by destructor and Client::stop
    Cancel _stopped;
};

}} // namespaces
