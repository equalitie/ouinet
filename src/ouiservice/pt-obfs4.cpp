#include "pt-obfs4.h"
#include "pluggable-transports/client-process.h"
#include "pluggable-transports/server-process.h"
#include "pluggable-transports/socks5-client.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../util.h"

namespace ouinet {
namespace ouiservice {

Obfs4OuiServiceServer::Obfs4OuiServiceServer(
    asio::io_service& ios,
    asio::ip::tcp::endpoint endpoint,
    fs::path state_directory
):
    PtOuiServiceServer(ios),
    _endpoint(endpoint),
    _state_directory(state_directory)
{}

std::unique_ptr<pt::ServerProcess> Obfs4OuiServiceServer::start_server_process(
    asio::io_service& ios,
    asio::ip::tcp::endpoint destination_endpoint,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    auto server_process = std::make_unique<pt::ServerProcess>(
        ios,
        "obfs4proxy",
        std::vector<std::string>(),
        "obfs4",
        _endpoint,
        destination_endpoint,
        std::map<std::string, std::string>(),
        _state_directory.string()
    );

    sys::error_code ec;
    server_process->start(yield[ec], cancel_signal);

    if (ec) {
        return or_throw<std::unique_ptr<pt::ServerProcess>>(yield, ec);
    }
    return std::move(server_process);
}



Obfs4OuiServiceClient::Obfs4OuiServiceClient(
    asio::io_service& ios,
    Obfs4Endpoint endpoint,
    fs::path state_directory
):
    PtOuiServiceClient(ios),
    _endpoint(endpoint),
    _state_directory(state_directory)
{}

std::unique_ptr<pt::ClientProcess> Obfs4OuiServiceClient::start_client_process(
    asio::io_service& ios,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    auto client_process = std::make_unique<pt::ClientProcess>(
        ios,
        "obfs4proxy",
        std::vector<std::string>(),
        "obfs4",
        _state_directory.string()
    );

    sys::error_code ec;
    client_process->start(yield[ec], cancel_signal);

    if (ec) {
        return or_throw<std::unique_ptr<pt::ClientProcess>>(yield, ec);
    }

    if (client_process->connection_method() != pt::ClientProcess::Socks5Connection) {
        return or_throw<std::unique_ptr<pt::ClientProcess>>(yield, asio::error::address_family_not_supported);
    }

    return std::move(client_process);
}

asio::ip::tcp::socket Obfs4OuiServiceClient::connect_through_transport(
    asio::io_service& ios,
    asio::ip::tcp::endpoint transport_endpoint,
    std::string& remote_endpoint_string,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    std::map<std::string, std::string> connection_arguments;
    connection_arguments["cert"] = _endpoint.certificate;
    connection_arguments["iat-mode"] = _endpoint.iat_mode;

    remote_endpoint_string = util::str(_endpoint.endpoint);

    return pt::connect_socks5(
        transport_endpoint,
        _endpoint.endpoint,
        connection_arguments,
        ios,
        yield,
        cancel_signal
    );
}

} // ouiservice namespace
} // ouinet namespace
