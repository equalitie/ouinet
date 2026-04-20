#pragma once

#include "../../ouiservice.h"

#include "address.h"
#include "tunnel.h"

namespace i2p::data {
    class PrivateKeys;
}

namespace i2p::client {
    class I2PServerTunnel;
    class ClientDestination;
}

namespace ouinet::ouiservice::i2poui {

class Service;

class Server : public ouinet::OuiServiceImplementationServer {
    using executor_type = asio::any_io_executor;
public:

    Server( std::shared_ptr<Service> service
          , const std::string& private_key_filename
            , uint32_t timeout, const executor_type&);

public:
    ~Server();

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    // Only used in tests
    GenericStream accept_without_handshake(asio::yield_context yield);

    I2pAddress public_identity() const;

    // Returns the server's ClientDestination, available after start_listen()
    std::shared_ptr<i2p::client::ClientDestination> get_destination() const { return _local_destination; }

    std::shared_ptr<Service> get_service() const { return _service; }
    executor_type get_executor() const { return _exec; }

private:
    void load_private_key(const std::string& key_file_name);

private:
    std::shared_ptr<Service> _service;
    executor_type _exec;
    std::unique_ptr<i2p::data::PrivateKeys> _private_keys;
    uint32_t _timeout;

    std::shared_ptr<i2p::client::ClientDestination> _local_destination;
    std::unique_ptr<Tunnel> _server_tunnel;
    asio::ip::tcp::acceptor _tcp_acceptor;

    // Triggered by destructor and Server::stop_listen
    Cancel _stopped;
};

} // namespaces
