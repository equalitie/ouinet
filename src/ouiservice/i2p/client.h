#pragma once

#include "../../ouiservice.h"

#include "tunnel.h"

namespace i2p::client {
    class I2PClientTunnel;
    class ClientDestination;
}

namespace ouinet::ouiservice::i2poui {

class Service;

class Client : public ouinet::OuiServiceImplementationClient {
public:
    Client( std::shared_ptr<Service> service
          , const std::string& target_id
          , uint32_t timeout
          , const AsioExecutor&
          , std::shared_ptr<i2p::client::ClientDestination> destination = nullptr);

    ~Client();

    AsioExecutor get_executor() { return _exec; }

    void start(asio::yield_context yield) override;
    void stop() override;

    GenericStream
    connect(asio::yield_context yield, Signal<void()>& cancel) override;

    // Returns the target I2P address this client connects to
    const std::string& get_target_id() const { return _target_id; }

    // Connect without the ouinet i2p handshake protocol. Use this for test  or for communicating
    // with non-ouinet I2P hosts such as BEP3 trackers.
    GenericStream
    connect_without_handshake(asio::yield_context yield, Signal<void()>& cancel);

private:
    std::shared_ptr<Service> _service;
    AsioExecutor _exec;
    std::string _target_id;
    uint32_t _timeout;

    // The destinaton where client requests originate from
    std::shared_ptr<i2p::client::ClientDestination> _destination;

    std::unique_ptr<Tunnel> _client_tunnel; //the tunnel is a pointer because
    //the client can be stopped (tunnel gets destroyed) and started again
    uint16_t _port;
    // Triggered by destructor and Client::stop
    Cancel _stopped;
};

} // namespaces
