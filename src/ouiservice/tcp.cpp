#include "tcp.h"
#include "../util.h"
#include "../logger.h"
#include "util/async.h"

namespace ouinet {
namespace ouiservice {

TcpOuiServiceServer::TcpOuiServiceServer(asio::any_io_executor ex, asio::ip::tcp::endpoint endpoint):
    _ex(std::move(ex)),
    _acceptor(_ex),
    _endpoint(endpoint)
{}

sys::error_code TcpOuiServiceServer::start_listen(Async)
{
    sys::error_code ec;

    _acceptor.open(_endpoint.protocol(), ec);
    if (ec) return ec;

    _acceptor.set_option(asio::socket_base::reuse_address(true));

    _acceptor.bind(_endpoint, ec);
    if (ec) {
        _acceptor.close();
        return ec;
    }

    _acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        _acceptor.close();
        return ec;
    }

    LOG_DEBUG("Successfully listening on TCP Port");  // used by integration tests

    return {};
}

void TcpOuiServiceServer::stop_listen()
{
    if (_acceptor.is_open()) {
        _acceptor.cancel();
        _acceptor.close();
    }
}

std::expected<GenericStream, sys::error_code> TcpOuiServiceServer::accept(Async yield)
{
    asio::ip::tcp::socket socket(_ex);
    auto r = _acceptor.async_accept(socket, yield);

    if (!r) return std::unexpected(r.error());

    static const auto tcp_shutter = [](asio::ip::tcp::socket& s) {
        sys::error_code ec;
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s.close(ec);
    };

    return GenericStream(std::move(socket), tcp_shutter);
}

TcpOuiServiceClient::TcpOuiServiceClient(asio::any_io_executor ex, asio::ip::tcp::endpoint endpoint):
    _ex(std::move(ex)),
    _endpoint(endpoint)
{}

std::expected<GenericStream, sys::error_code>
TcpOuiServiceClient::connect(Async yield)
{
    if (!_endpoint) {
        return std::unexpected(asio::error::invalid_argument);
    }

    asio::ip::tcp::socket socket(_ex);

    auto cancel_slot = yield.cancel_slot([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        sys::error_code ec;
        socket.close(ec);
    });

    auto r = socket.async_connect(*_endpoint, yield);

    if (!r) return std::unexpected(r.error());

    static const auto tcp_shutter = [](asio::ip::tcp::socket& s) {
        sys::error_code ec;
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        s.close(ec);
    };

    return GenericStream(std::move(socket), tcp_shutter);
}

sys::error_code TcpOuiServiceClient::start(Async) {
    return sys::error_code();
}

} // ouiservice namespace
} // ouinet namespace
