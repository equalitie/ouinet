#include "tcp.h"
#include "../or_throw.h"
#include "../util.h"
#include "../logger.h"

namespace ouinet {
namespace ouiservice {

TcpOuiServiceServer::TcpOuiServiceServer(asio::io_service& ios, asio::ip::tcp::endpoint endpoint):
    _ios(ios),
    _acceptor(ios),
    _endpoint(endpoint)
{}

void TcpOuiServiceServer::start_listen(asio::yield_context yield)
{
    sys::error_code ec;

    _acceptor.open(_endpoint.protocol(), ec);
    if (ec) {
        return or_throw(yield, ec);
    }

    _acceptor.set_option(asio::socket_base::reuse_address(true));

    _acceptor.bind(_endpoint, ec);
    if (ec) {
        _acceptor.close();
        return or_throw(yield, ec);
    }

    _acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) {
        _acceptor.close();
        return or_throw(yield, ec);
    }

    LOG_DEBUG("Successfully listening on TCP Port");

}

void TcpOuiServiceServer::stop_listen()
{
    if (_acceptor.is_open()) {
        _acceptor.cancel();
        _acceptor.close();
    }
}

GenericStream TcpOuiServiceServer::accept(asio::yield_context yield)
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

TcpOuiServiceClient::TcpOuiServiceClient(asio::io_service& ios, std::string endpoint):
    _ios(ios),
    _endpoint(parse_endpoint(endpoint))
{}

OuiServiceImplementationClient::ConnectInfo
TcpOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    if (!_endpoint) {
        return or_throw<ConnectInfo>(yield, asio::error::invalid_argument);
    }

    sys::error_code ec;

    asio::ip::tcp::socket socket(_ios);

    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        sys::error_code ec;
        socket.close(ec);
    });

    socket.async_connect(*_endpoint, yield[ec]);

    if (ec) {
        return or_throw<ConnectInfo>(yield, ec);
    }

    static const auto tcp_shutter = [](asio::ip::tcp::socket& s) {
        sys::error_code ec;
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s.close(ec);
    };

    return ConnectInfo{ GenericStream(std::move(socket), tcp_shutter)
                      , util::str(*_endpoint) };
}

} // ouiservice namespace
} // ouinet namespace
