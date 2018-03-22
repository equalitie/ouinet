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

    void start_listen(asio::yield_context yield);
    void stop_listen();

    GenericConnection accept(asio::yield_context yield);

    private:
    asio::io_service& _ios;
    asio::ip::tcp::acceptor _acceptor;
    asio::ip::tcp::endpoint _endpoint;
};

class TcpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    TcpOuiServiceClient(asio::io_service& ios, asio::ip::tcp::endpoint endpoint);

    void start(asio::yield_context yield) {}
    void stop() {}

    GenericConnection connect(asio::yield_context yield, Signal<void()>& cancel);

    private:
    asio::io_service& _ios;
    asio::ip::tcp::endpoint _endpoint;
};

} // ouiservice namespace
} // ouinet namespace
