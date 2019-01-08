#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem.hpp>

#include "../endpoint.h"
#include "../ouiservice.h"

#include "pluggable-transports/pt-ouiservice.h"

namespace ouinet {
namespace ouiservice {

class Obfs4OuiServiceServer : public pt::PtOuiServiceServer
{
    public:
    Obfs4OuiServiceServer(
        asio::io_service& ios,
        asio::ip::tcp::endpoint endpoint,
        fs::path state_directory
    );

    protected:
    std::unique_ptr<pt::ServerProcess> start_server_process(
        asio::io_service& ios,
        asio::ip::tcp::endpoint destination_endpoint,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) override;

    private:
    asio::ip::tcp::endpoint _endpoint;
    fs::path _state_directory;
};

class Obfs4OuiServiceClient : public pt::PtOuiServiceClient
{
    public:
    Obfs4OuiServiceClient(
        asio::io_service& ios,
        Obfs4Endpoint endpoint,
        fs::path state_directory
    );

    std::unique_ptr<pt::ClientProcess> start_client_process(
        asio::io_service& ios,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) override;
    asio::ip::tcp::socket connect_through_transport(
        asio::io_service& ios,
        asio::ip::tcp::endpoint transport_endpoint,
        std::string& remote_endpoint_string,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) override;

    private:
    Obfs4Endpoint _endpoint;
    fs::path _state_directory;
};

} // ouiservice namespace
} // ouinet namespace
