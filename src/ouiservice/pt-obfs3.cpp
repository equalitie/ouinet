#include "pt-obfs3.h"
#include "pluggable-transports/client-process.h"
#include "pluggable-transports/server-process.h"
#include "pluggable-transports/socks5-client.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../util.h"

#include <boost/algorithm/string.hpp>

namespace ouinet {
namespace ouiservice {

Obfs3OuiServiceServer::Obfs3OuiServiceServer(
    asio::io_service& ios,
    asio::ip::tcp::endpoint endpoint,
    fs::path state_directory
):
    PtOuiServiceServer(ios),
    _endpoint(endpoint),
    _state_directory(state_directory)
{}

std::unique_ptr<pt::ServerProcess> Obfs3OuiServiceServer::start_server_process(
    asio::io_service& ios,
    asio::ip::tcp::endpoint destination_endpoint,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    auto server_process = std::make_unique<pt::ServerProcess>(
        ios,
        "obfs4proxy",
        std::vector<std::string>(),
        "obfs3",
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

    return server_process;
}



static boost::optional<asio::ip::tcp::endpoint> parse_endpoint(std::string endpoint)
{
    size_t pos = endpoint.rfind(':');
    if (pos == std::string::npos) {
        return boost::none;
    }

    int port;
    try {
        port = std::stoi(endpoint.substr(pos + 1));
    } catch(...) {
        return boost::none;
    }
    sys::error_code ec;
    asio::ip::address address = asio::ip::address::from_string(endpoint.substr(0, pos), ec);
    if (ec) {
        return boost::none;
    }
    return asio::ip::tcp::endpoint(address, port);
}

Obfs3OuiServiceClient::Obfs3OuiServiceClient(
    asio::io_service& ios,
    std::string endpoint,
    fs::path state_directory
):
    PtOuiServiceClient(ios),
    _endpoint(parse_endpoint(endpoint)),
    _state_directory(state_directory)
{}

std::unique_ptr<pt::ClientProcess> Obfs3OuiServiceClient::start_client_process(
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
        "obfs3",
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

    return client_process;
}

asio::ip::tcp::socket Obfs3OuiServiceClient::connect_through_transport(
    asio::io_service& ios,
    asio::ip::tcp::endpoint transport_endpoint,
    std::string& remote_endpoint_string,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    remote_endpoint_string = util::str(*_endpoint);

    return pt::connect_socks5(
        transport_endpoint,
        *_endpoint,
        boost::none,
        ios,
        yield,
        cancel_signal
    );
}

} // ouiservice namespace
} // ouinet namespace
