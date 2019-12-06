#pragma once

#include "../../ouiservice.h"

#include "tunnel.h"

namespace i2p { namespace client {
    class I2PClientTunnel;
}}

namespace ouinet {
namespace ouiservice {
namespace i2poui {

class Service;

class Client : public ouinet::OuiServiceImplementationClient {
private:
    // Client is constructed by i2poui::Service
    friend class Service;
    Client( std::shared_ptr<Service> service
          , const std::string& target_id
          , uint32_t timeout
          , const asio::executor&);

public:
    ~Client();

    asio::executor get_executor() { return _exec; }

    void start(asio::yield_context yield) override;
    void stop() override;

    GenericStream
    connect(asio::yield_context yield, Signal<void()>& cancel) override;

private:
    std::shared_ptr<Service> _service;
    asio::executor _exec;
    std::string _target_id;
    uint32_t _timeout;

    std::unique_ptr<Tunnel> _client_tunnel; //the tunnel is a pointer because
    //the client can be stopped (tunnel gets destroyed) and started again
    uint16_t _port;
};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
