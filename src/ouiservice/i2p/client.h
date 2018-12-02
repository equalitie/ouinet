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
          , asio::io_service& ios);

public:
    ~Client();

    asio::io_service& get_io_service() { return _ios; }

    void start(asio::yield_context yield) override;
    void stop() override;


    void shake_hand(asio::yield_context yield);
    
    ouinet::OuiServiceImplementationClient::ConnectInfo
    connect(asio::yield_context yield, Signal<void()>& cancel) override;

private:
    std::shared_ptr<Service> _service;
    asio::io_service& _ios;
    std::string _target_id;
    uint32_t _timeout;

    std::unique_ptr<Tunnel> _client_tunnel; //the tunnel is a pointer because
    //the client can be stopped (tunnel gets destroyed) and started again
    uint16_t _port;

    //handshake stuff
    bool _handshaken = false; //indicate that we have handshaken with the server over the tunnel.
    const std::string c_handshaking_host = "example.org"; //we example.com and .net have special treatment
    const std::string c_handshaking_url = "http://example.org/"; //we example.com and .net have special treatment

};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
