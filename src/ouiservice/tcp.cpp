#include "tcp.h"
#include "../or_throw.h"

namespace ouinet {
namespace ouiservice {

TcpOuiServiceServer::TcpOuiServiceServer(boost::asio::io_service& ios, boost::asio::ip::tcp::endpoint endpoint):
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

    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));

    _acceptor.bind(_endpoint, ec);
    if (ec) {
        _acceptor.close();
        return or_throw(yield, ec);
    }

    _acceptor.listen(boost::asio::socket_base::max_connections, ec);
    if (ec) {
        _acceptor.close();
        return or_throw(yield, ec);
    }
}

void TcpOuiServiceServer::stop_listen()
{
    if (_acceptor.is_open()) {
        _acceptor.cancel();
        _acceptor.close();
    }
}

GenericConnection TcpOuiServiceServer::accept(asio::yield_context yield)
{
    sys::error_code ec;

    boost::asio::ip::tcp::socket socket(_ios);
    _acceptor.async_accept(socket, yield[ec]);

    if (ec) {
        return or_throw<GenericConnection>(yield, ec, GenericConnection());
    }

    return GenericConnection(std::move(socket));
}

TcpOuiServiceClient::TcpOuiServiceClient(boost::asio::io_service& ios, boost::asio::ip::tcp::endpoint endpoint):
    _ios(ios),
    _endpoint(endpoint)
{}

GenericConnection TcpOuiServiceClient::connect(asio::yield_context yield)
{
    sys::error_code ec;

    boost::asio::ip::tcp::socket socket(_ios);
    auto it = _connecting_sockets.insert(_connecting_sockets.end(), &socket);
    socket.async_connect(_endpoint, yield[ec]);
    _connecting_sockets.erase(it);

    if (ec) {
        return or_throw<GenericConnection>(yield, ec, GenericConnection());
    }

    return GenericConnection(std::move(socket));
}

void TcpOuiServiceClient::cancel_connect()
{
    for (auto& socket : _connecting_sockets) {
        // tcp::socket::cancel() does not work properly on all platforms
        socket->close();
    }
}

} // ouiservice namespace
} // ouinet namespace
