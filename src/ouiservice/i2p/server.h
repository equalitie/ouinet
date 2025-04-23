#pragma once

#include "../../ouiservice.h"

#include "tunnel.h"

namespace i2p::data {
    class PrivateKeys;
}

namespace i2p::client {
    class I2PServerTunnel;
}

namespace ouinet::ouiservice::i2poui {

class Service;

class Server : public ouinet::OuiServiceImplementationServer {
public:

    Server( std::shared_ptr<Service> service
          , const std::string& private_key_filename
            , uint32_t timeout, const AsioExecutor&);

public:
    ~Server();

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    // Only used in tests
    GenericStream accept_without_handshake(asio::yield_context yield);

    std::string public_identity() const;

private:
    void load_private_key(const std::string& key_file_name);

private:
    std::shared_ptr<Service> _service;
    AsioExecutor _exec;
    std::unique_ptr<i2p::data::PrivateKeys> _private_keys;
    uint32_t _timeout;

    std::unique_ptr<Tunnel> _server_tunnel;
    asio::ip::tcp::acceptor _tcp_acceptor;

    // Triggered by destructor and Server::stop_listen
    Cancel _stopped;
};

} // namespaces
