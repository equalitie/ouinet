#pragma once

#include <list>
#include <boost/asio/ip/tcp.hpp>
#include <boost/optional.hpp>

#include "../ouiservice.h"

namespace ouinet {

class Async;

namespace ouiservice {

class TcpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    TcpOuiServiceServer(asio::any_io_executor, asio::ip::tcp::endpoint endpoint);

    [[nodiscard]]
    sys::error_code start_listen(Async) override;

    void stop_listen() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async) override;

    private:
    asio::any_io_executor _ex;
    asio::ip::tcp::acceptor _acceptor;
    asio::ip::tcp::endpoint _endpoint;
};

class TcpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    TcpOuiServiceClient(asio::any_io_executor, asio::ip::tcp::endpoint endpoint);

    // Tcp clients don't have any internal async IO to be started/stopped.
    [[nodiscard]]
    sys::error_code start(Async) override;
    void stop() override {}

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async) override;

    bool verify_endpoint() const { return (bool)_endpoint; }

    private:
    asio::any_io_executor _ex;
    boost::optional<asio::ip::tcp::endpoint> _endpoint;
};

} // ouiservice namespace
} // ouinet namespace
