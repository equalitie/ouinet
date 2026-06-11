#include "utp.h"
#include "../async_sleep.h"
#include "../logger.h"
#include "../parse/endpoint.h"
#include "../util/async.h"
#include "../util/select.h"
#include "../util/str.h"

namespace ouinet {
namespace ouiservice {

using udp = asio::ip::udp;
using namespace std;

UtpOuiServiceServer::UtpOuiServiceServer( asio::any_io_executor ex
                                        , udp::endpoint local_endpoint):
    _ex(std::move(ex)),
    _udp_multiplexer(new asio_utp::udp_multiplexer(_ex)),
    _accept_queue(_ex)
{
    sys::error_code ec;

    _udp_multiplexer->bind(local_endpoint, ec);

    assert(_udp_multiplexer->is_open());
    if (ec) {
        LOG_ERROR("uTP: Failed to bind UtpOuiServiceServer to "
                 , local_endpoint, "; ec=", ec);
    } else {
        LOG_DEBUG("uTP UDP endpoint: ", _udp_multiplexer->local_endpoint());
    }
}

sys::error_code UtpOuiServiceServer::start_listen(Async yield)
{
    using namespace std::chrono_literals;

    assert(_udp_multiplexer->is_open());
    yield.spawn(_cancel, [this] (Async yield) {
        auto local_ep = _udp_multiplexer->local_endpoint();

        while (true) {
            sys::error_code ec;
            asio_utp::socket s(_ex);

            auto cancel_con = yield.cancel_slot([&] { s.close(); });

            s.bind(local_ep, ec);
            assert(!ec);
            ec = s.async_accept(yield);
            if (ec) {
                LOG_ERROR("UtpOuiServiceServer: failed to accept, will retry in 5s;"
                         , " lep=", local_ep, " ec=", ec);
                async_sleep(5s, yield);
                continue;
            }

            auto ep = util::str("uTP/", s.remote_endpoint());
            ec = _accept_queue.async_send(sys::error_code(), {move(s), move(ep)}, yield);
            if (ec) break;
        }
    });

    return sys::error_code();
}

void UtpOuiServiceServer::stop_listen()
{
    _cancel();
}

UtpOuiServiceServer::~UtpOuiServiceServer()
{
    stop_listen();
}

std::expected<GenericStream, sys::error_code> UtpOuiServiceServer::accept(Async yield)
{
    auto s = _accept_queue.async_receive(yield);
    if (!s.has_value()) {
        return std::unexpected(s.error());
    }
    return move(*s);
}

UtpOuiServiceClient::UtpOuiServiceClient( asio::any_io_executor ex
                                        , asio_utp::udp_multiplexer m
                                        , asio::ip::udp::endpoint endpoint):
    _ex(std::move(ex)),
    _remote_endpoint(endpoint),
    _udp_multiplexer(move(m))
{
}

sys::error_code UtpOuiServiceClient::start(Async) {
    return sys::error_code();
}

std::expected<GenericStream, sys::error_code>
UtpOuiServiceClient::connect(Async yield)
{
    using namespace chrono_literals;

    if (!_remote_endpoint) {
        return std::unexpected(asio::error::invalid_argument);
    }

    sys::error_code ec;
    asio_utp::socket socket;

    static const chrono::seconds retry_timeout[] = { 4s , 8s , 16s };

    for (int i = 0; i != sizeof(retry_timeout)/sizeof(*retry_timeout); ++i) {
        ec = sys::error_code();

        socket = asio_utp::socket(_ex);
        socket.bind(_udp_multiplexer, ec);
        assert(!ec);

        auto result = timeout(
            retry_timeout[i],
            [&](auto yield) { return socket.async_connect(*_remote_endpoint, yield); },
            yield
        );

        if (!result) {
            ec = result.error();
        } else {
            break;
        }
    }

    if (ec) {
        return std::unexpected(ec);
    }

    static const auto shutter = [](asio_utp::socket& s) {
        s.close();
    };

    return GenericStream(std::move(socket), shutter);
}

} // ouiservice namespace
} // ouinet namespace
