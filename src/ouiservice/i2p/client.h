#pragma once

#include "../../ouiservice.h"

#include "tunnel.h"

namespace i2p::client {
    class I2PClientTunnel;
}

namespace ouinet::ouiservice::i2poui {

class Service;

class Client : public ouinet::OuiServiceImplementationClient {
public:
    Client( std::shared_ptr<Service> service
          , const std::string& target_id
          , uint32_t timeout
          , const AsioExecutor&);

    ~Client();

    AsioExecutor get_executor() { return _exec; }

    void start(asio::yield_context yield) override;
    void stop() override;

    GenericStream
    connect(asio::yield_context yield, Signal<void()>& cancel) override;

    // Used only in tests
    GenericStream
    connect_without_handshake(asio::yield_context yield, Signal<void()>& cancel);

private:
    std::shared_ptr<Service> _service;
    AsioExecutor _exec;
    std::string _target_id;
    uint32_t _timeout;

    std::unique_ptr<Tunnel> _client_tunnel; //the tunnel is a pointer because
    //the client can be stopped (tunnel gets destroyed) and started again
    uint16_t _port;
    // Triggered by destructor and Client::stop
    Cancel _stopped;
};

} // namespaces
