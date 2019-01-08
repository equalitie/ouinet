#include "pt-obfs4.h"
#include "pluggable-transports/client-process.h"
#include "pluggable-transports/server-process.h"
#include "pluggable-transports/socks5-client.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../util.h"

#include <boost/algorithm/string.hpp>

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



static void parse_endpoint(
    std::string endpoint_string,
    boost::optional<asio::ip::tcp::endpoint>& endpoint,
    std::string& certificate,
    std::string& iat_mode
) {
    endpoint = boost::none;
    std::vector<std::string> parts;
    boost::algorithm::split(parts, endpoint_string, [](char c) { return c == ','; });
    if (parts.size() != 3) {
        return;
    }
    if (parts[1].substr(0, 5) != "cert=") {
        return;
    }
    certificate = parts[1].substr(5);
    if (parts[2].substr(0, 9) != "iat-mode=") {
        return;
    }
    iat_mode = parts[2].substr(9);


    size_t pos = parts[0].rfind(':');
    if (pos == std::string::npos) {
        return;
    }

    int port;
    try {
        port = std::stoi(parts[0].substr(pos + 1));
    } catch(...) {
        return;
    }
    sys::error_code ec;
    asio::ip::address address = asio::ip::address::from_string(parts[0].substr(0, pos), ec);
    if (ec) {
        return;
    }
    endpoint = asio::ip::tcp::endpoint(address, port);
}

Obfs4OuiServiceClient::Obfs4OuiServiceClient(
    asio::io_service& ios,
    std::string endpoint,
    fs::path state_directory
):
    PtOuiServiceClient(ios),
    _state_directory(state_directory)
{
    parse_endpoint(endpoint, _endpoint, _certificate, _iat_mode);
}

std::unique_ptr<pt::ClientProcess> Obfs4OuiServiceClient::start_client_process(
    asio::io_service& ios,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    if (!_endpoint) {
        return or_throw<std::unique_ptr<pt::ClientProcess>>(yield, asio::error::invalid_argument);
    }

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
    connection_arguments["cert"] = _certificate;
    connection_arguments["iat-mode"] = _iat_mode;

    remote_endpoint_string = util::str(*_endpoint);

    return pt::connect_socks5(
        transport_endpoint,
        *_endpoint,
        connection_arguments,
        ios,
        yield,
        cancel_signal
    );
}

} // ouiservice namespace
} // ouinet namespace
