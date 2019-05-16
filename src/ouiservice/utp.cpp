#include "utp.h"
#include "../or_throw.h"
#include "../util.h"
#include "../logger.h"

namespace ouinet {
namespace ouiservice {

using namespace std;

UtpOuiServiceServer::UtpOuiServiceServer(asio::io_service& ios, asio::ip::udp::endpoint endpoint):
    _ios(ios),
    _endpoint(endpoint),
    _accept_queue(_ios)
{}

void UtpOuiServiceServer::start_listen(asio::yield_context yield)
{
    asio::spawn(_ios, [&] (asio::yield_context yield) {
        Cancel cancel(_cancel);

        while (!cancel) {
            sys::error_code ec;
            asio_utp::socket s(_ios, _endpoint);
            s.async_accept(yield[ec]);
            if (cancel) return;
            assert(!ec);
            _accept_queue.async_push(move(s), ec, cancel, yield[ec]);
        }
    });
}

void UtpOuiServiceServer::stop_listen()
{
    _cancel();
}

UtpOuiServiceServer::~UtpOuiServiceServer()
{
    stop_listen();
}

GenericStream UtpOuiServiceServer::accept(asio::yield_context yield)
{
    sys::error_code ec;
    auto s = _accept_queue.async_pop(_cancel, yield[ec]);
    return or_throw(yield, ec, move(s));
}

static boost::optional<asio::ip::udp::endpoint> parse_endpoint(std::string endpoint)
{
    sys::error_code ec;
    auto ep = util::parse_endpoint<asio::ip::udp>(endpoint, ec);
    if (ec) return boost::none;
    return ep;
}

UtpOuiServiceClient::UtpOuiServiceClient(asio::io_service& ios, std::string endpoint):
    _ios(ios),
    _endpoint(parse_endpoint(endpoint))
{}

GenericStream
UtpOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    if (!_endpoint) {
        return or_throw<GenericStream>(yield, asio::error::invalid_argument);
    }

    sys::error_code ec;

    asio_utp::socket socket(_ios, {asio::ip::address_v4::any(), 0});

    auto cancel_slot = cancel.connect([&] {
        socket.close();
    });

    socket.async_connect(*_endpoint, yield[ec]);

    if (cancel) ec = asio::error::operation_aborted;

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    static const auto shutter = [](asio_utp::socket& s) {
        s.close();
    };

    return GenericStream(std::move(socket), shutter);
}

} // ouiservice namespace
} // ouinet namespace
