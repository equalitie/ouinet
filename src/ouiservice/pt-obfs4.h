#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include "../ouiservice.h"

#include "pluggable-transports/pt-ouiservice.h"

namespace ouinet {
namespace ouiservice {

class Obfs4OuiServiceServer : public pt::PtOuiServiceServer
{
    public:
    Obfs4OuiServiceServer(
        asio::io_context&,
        asio::ip::tcp::endpoint endpoint,
        fs::path state_directory
    );

    protected:
    std::unique_ptr<pt::ServerProcess> start_server_process(
        asio::io_context&,
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
        asio::io_context&,
        std::string endpoint,
        fs::path state_directory
    );

    bool verify_endpoint() const { return (bool)_endpoint; }

    protected:
    std::unique_ptr<pt::ClientProcess> start_client_process(
        asio::io_context&,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) override;
    asio::ip::tcp::socket connect_through_transport(
        const asio::executor&,
        asio::ip::tcp::endpoint transport_endpoint,
        std::string& remote_endpoint_string,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) override;

    private:
    boost::optional<asio::ip::tcp::endpoint> _endpoint;
    std::string _certificate;
    std::string _iat_mode;
    fs::path _state_directory;
};

} // ouiservice namespace
} // ouinet namespace
