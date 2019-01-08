#pragma once

#include <boost/asio/ip/tcp.hpp>
#include "../../ouiservice.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

class ClientProcess;
class ServerProcess;

class PtOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    PtOuiServiceServer(asio::io_service& ios);
    ~PtOuiServiceServer();

    void start_listen(asio::yield_context yield) final;
    void stop_listen() final;

    GenericStream accept(asio::yield_context yield) final;

    std::string connection_arguments() const;

    protected:
    virtual std::unique_ptr<ServerProcess> start_server_process(
        asio::io_service& ios,
        asio::ip::tcp::endpoint destination_endpoint,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) = 0;

    private:
    asio::io_service& _ios;

    asio::ip::tcp::acceptor _acceptor;
    std::unique_ptr<pt::ServerProcess> _server_process;
};

class PtOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    PtOuiServiceClient(asio::io_service& ios);
    ~PtOuiServiceClient();

    void start(asio::yield_context yield) final;
    void stop() final;

    OuiServiceImplementationClient::ConnectInfo connect(
        asio::yield_context yield,
        Signal<void()>& cancel
    ) final;

    protected:
    virtual std::unique_ptr<ClientProcess> start_client_process(
        asio::io_service& ios,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) = 0;
    virtual asio::ip::tcp::socket connect_through_transport(
        asio::io_service& ios,
        asio::ip::tcp::endpoint transport_endpoint,
        std::string& remote_endpoint_string,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) = 0;

    private:
    asio::io_service& _ios;

    std::unique_ptr<pt::ClientProcess> _client_process;
};

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
