#pragma once

#include <list>
#include <boost/asio/ip/tcp.hpp>

#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

class TcpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    TcpOuiServiceServer(asio::io_service& ios, asio::ip::tcp::endpoint endpoint);

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    private:
    asio::io_service& _ios;
    asio::ip::tcp::acceptor _acceptor;
    asio::ip::tcp::endpoint _endpoint;
};

class TcpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    using ConnectInfo = OuiServiceImplementationClient::ConnectInfo;

    public:
    TcpOuiServiceClient(asio::io_service& ios, asio::ip::tcp::endpoint endpoint);

    // Tcp clients don't have any internal async IO to be started/stopped.
    void start(asio::yield_context yield) override {}
    void stop() override {}

    ConnectInfo connect( asio::yield_context yield
                       , Signal<void()>& cancel) override;

    private:
    asio::io_service& _ios;
    asio::ip::tcp::endpoint _endpoint;
};

} // ouiservice namespace
} // ouinet namespace
