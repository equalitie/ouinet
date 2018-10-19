#pragma once

#include "../../ouiservice.h"

#include "tunnel.h"

namespace i2p { namespace data {
    class PrivateKeys;
}}

namespace i2p { namespace client {
    class I2PServerTunnel;
}}

namespace ouinet {
namespace ouiservice {
namespace i2poui {

class Service;

class Server : public ouinet::OuiServiceImplementationServer {
private:
    // Client is constructed by i2poui::Service
    friend class Service;

    Server( std::shared_ptr<Service> service
          , const std::string& private_key_filename
            , uint32_t timeout, asio::io_service& ios);

    void load_private_key(const std::string& key_file_name);

public:
    ~Server();

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    std::string public_identity() const;

private:
    std::shared_ptr<Service> _service;
    asio::io_service& _ios;
    std::unique_ptr<i2p::data::PrivateKeys> _private_keys;
    uint32_t _timeout;

    std::unique_ptr<Tunnel> _server_tunnel;
    asio::ip::tcp::acceptor _tcp_acceptor;
};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
