#include "utp.h"
#include "../or_throw.h"
#include "../parse/endpoint.h"
#include "../logger.h"
#include "../util/watch_dog.h"
#include "../util/handler_tracker.h"

namespace ouinet {
namespace ouiservice {

using udp = asio::ip::udp;
using namespace std;

UtpOuiServiceServer::UtpOuiServiceServer( const asio::executor& ex
                                        , udp::endpoint local_endpoint):
    _ex(ex),
    _udp_multiplexer(new asio_utp::udp_multiplexer(_ex)),
    _accept_queue(_ex)
{
    sys::error_code ec;

    _udp_multiplexer->bind(local_endpoint, ec);

    if (ec) {
        LOG_ERROR("uTP: Failed to bind UtpOuiServiceServer to "
                 , local_endpoint, " ec:", ec.message());
    } else {
        LOG_DEBUG("uTP UDP endpoint:", _udp_multiplexer->local_endpoint());
    }
}

void UtpOuiServiceServer::start_listen(asio::yield_context yield)
{
    TRACK_SPAWN(_ex, [&] (asio::yield_context yield) {
        Cancel cancel(_cancel);

        while (!cancel) {
            sys::error_code ec;
            asio_utp::socket s(_ex);

            auto cancel_con = cancel.connect([&] { s.close(); });

            s.bind(_udp_multiplexer->local_endpoint(), ec);
            assert(!ec);
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
    auto ep = parse::endpoint<asio::ip::udp>(endpoint, ec);
    if (ec) return boost::none;
    return ep;
}

UtpOuiServiceClient::UtpOuiServiceClient( const asio::executor& ex
                                        , asio_utp::udp_multiplexer m
                                        , std::string endpoint):
    _ex(ex),
    _remote_endpoint(parse_endpoint(endpoint)),
    _udp_multiplexer(move(m))
{
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

    static const chrono::seconds retry_timeout[] = { 4s , 8s , 16s };

    for (int i = 0; i != sizeof(retry_timeout)/sizeof(*retry_timeout); ++i) {
        ec = sys::error_code();

        socket = asio_utp::socket(_ex);
        socket.bind(_udp_multiplexer, ec);
        assert(!ec);

        auto cancel_slot = cancel.connect([&] {
            socket.close();
        });

        bool timed_out = false;

        WatchDog wd(_ex, retry_timeout[i], [&] {
                timed_out = true;
                socket.close();
        });

        socket.async_connect(*_remote_endpoint, yield[ec]);

        if (!timed_out) break;
        ec = asio::error::timed_out;
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
