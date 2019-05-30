#include "utp.h"
#include "../or_throw.h"
#include "../util.h"
#include "../logger.h"
#include "../util/watch_dog.h"

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
    _remote_endpoint(parse_endpoint(endpoint)),
    _udp_multiplexer(
            new asio_utp::udp_multiplexer(_ios
                                         , asio::ip::udp::endpoint
                                            ( asio::ip::address_v4::any()
                                            , 0 )))
{
    cerr << "uTP local endpoint is: UDP:" << _udp_multiplexer->local_endpoint() << "\n";
}

GenericStream
UtpOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    using namespace chrono_literals;

    if (!_remote_endpoint) {
        return or_throw<GenericStream>(yield, asio::error::invalid_argument);
    }

    sys::error_code ec;
    asio_utp::socket socket;

    const size_t max_retries = 3;

    static const chrono::milliseconds retry_timeout[max_retries] = { 1000ms
                                                                   , 2000ms
                                                                   , 5000ms };

    for (int i = 0; i != max_retries; ++i) {
        ec = sys::error_code();

        socket = asio_utp::socket(_ios, _udp_multiplexer->local_endpoint());

        auto cancel_slot = cancel.connect([&] {
            socket.close();
        });

        bool timed_out = false;

        WatchDog wd(_ios, retry_timeout[i], [&] {
                timed_out = true;
                socket.close();
        });

        socket.async_connect(*_remote_endpoint, yield[ec]);

        if (!timed_out) break;
    }

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
