#pragma once

#include <list>
#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>

#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

class TcpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    TcpOuiServiceServer(const asio::executor&, asio::ip::tcp::endpoint endpoint);

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    private:
    asio::executor _ex;
    asio::ip::tcp::acceptor _acceptor;
    asio::ip::tcp::endpoint _endpoint;
};

class TcpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    TcpOuiServiceClient(const asio::executor&, std::string endpoint);

    // Tcp clients don't have any internal async IO to be started/stopped.
    void start(asio::yield_context yield) override {}
    void stop() override {}

    GenericStream connect( asio::yield_context yield
                         , Signal<void()>& cancel) override;

    bool verify_endpoint() const { return (bool)_endpoint; }

    private:
    asio::executor _ex;
    boost::optional<asio::ip::tcp::endpoint> _endpoint;
};

} // ouiservice namespace
} // ouinet namespace
