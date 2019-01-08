#include "pt-ouiservice.h"
#include "client-process.h"
#include "server-process.h"
#include "socks5-client.h"
#include "../../or_throw.h"
#include "../../util.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

PtOuiServiceServer::PtOuiServiceServer(asio::io_service& ios):
    _ios(ios),
    _acceptor(ios)
{}

PtOuiServiceServer::~PtOuiServiceServer()
{}

void PtOuiServiceServer::start_listen(asio::yield_context yield)
{
    if (_server_process) {
        return or_throw(yield, asio::error::in_progress);
    }

    asio::ip::tcp::endpoint tcp_endpoint(
        asio::ip::address_v4::loopback(),
        0
    );

    sys::error_code ec;

    _acceptor.open(tcp_endpoint.protocol(), ec);
    if (ec) {
        return or_throw(yield, ec);
    }

    _acceptor.set_option(asio::socket_base::reuse_address(true));

    _acceptor.bind(tcp_endpoint, ec);
    if (ec) {
        _acceptor.close();
        return or_throw(yield, ec);
    }

    _acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) {
        _acceptor.close();
        return or_throw(yield, ec);
    }

    Signal<void()> cancel;
    _server_process = start_server_process(
        _ios,
        _acceptor.local_endpoint(),
        yield[ec],
        cancel
    );

    if (ec) {
        _acceptor.cancel();
        _acceptor.close();
        _server_process.reset();
        return or_throw(yield, ec);
    }
}

void PtOuiServiceServer::stop_listen()
{
    if (_server_process) {
        _server_process.reset();
        _acceptor.cancel();
        _acceptor.close();
    }
}

GenericStream PtOuiServiceServer::accept(asio::yield_context yield)
{
    sys::error_code ec;

    asio::ip::tcp::socket socket(_ios);
    _acceptor.async_accept(socket, yield[ec]);

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    static const auto tcp_shutter = [](asio::ip::tcp::socket& s) {
        sys::error_code ec;
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s.close(ec);
    };

    return GenericStream(std::move(socket), tcp_shutter);
}

std::string PtOuiServiceServer::connection_arguments() const
{
    return _server_process->connection_arguments();
}



PtOuiServiceClient::PtOuiServiceClient(asio::io_service& ios):
    _ios(ios)
{}

PtOuiServiceClient::~PtOuiServiceClient()
{}

void PtOuiServiceClient::start(asio::yield_context yield)
{
    if (_client_process) {
        return or_throw(yield, asio::error::in_progress);
    }

    sys::error_code ec;
    Signal<void()> cancel;
    _client_process = start_client_process(
        _ios,
        yield[ec],
        cancel
    );
    if (ec) {
        _client_process.reset();
        return or_throw(yield, ec);
    }
}

void PtOuiServiceClient::stop()
{
    _client_process.reset();
}

OuiServiceImplementationClient::ConnectInfo PtOuiServiceClient::connect(
    asio::yield_context yield,
    Signal<void()>& cancel
) {
    if (!_client_process) {
        return or_throw<OuiServiceImplementationClient::ConnectInfo>(yield, asio::error::not_connected);
    }

    sys::error_code ec;
    std::string remote_endpoint_string;
    asio::ip::tcp::socket socket = connect_through_transport(
        _ios,
        _client_process->endpoint(),
        remote_endpoint_string,
        yield[ec],
        cancel
    );

    if (ec) {
        return or_throw<OuiServiceImplementationClient::ConnectInfo>(yield, ec);
    }

    static const auto tcp_shutter = [](asio::ip::tcp::socket& s) {
        sys::error_code ec;
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s.close(ec);
    };

    return ConnectInfo{
        GenericStream(std::move(socket), tcp_shutter),
        remote_endpoint_string
    };
}

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
