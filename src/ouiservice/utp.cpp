#include "utp.h"
#include "../async_sleep.h"
#include "../logger.h"
#include "../parse/endpoint.h"
#include "../util/async.h"
#include "../util/select.h"
#include "../util/str.h"
#include "asio_utp/udp_multiplexer.hpp"

namespace ouinet {
namespace ouiservice {

using udp = asio::ip::udp;
using namespace std;

UtpOuiServiceServer::UtpOuiServiceServer( asio_utp::udp_multiplexer mux, util::LogPath log_path):
    _udp_multiplexer(std::make_unique<asio_utp::udp_multiplexer>(std::move(mux))),
    _accept_queue(_udp_multiplexer->get_executor())
{
    assert(_udp_multiplexer->is_open());
}

sys::error_code UtpOuiServiceServer::start_listen(Async yield)
{
    using namespace std::chrono_literals;

    assert(_udp_multiplexer->is_open());
    yield.spawn(_cancel, [this] (Async yield) {
        auto exec = _udp_multiplexer->get_executor();

        while (true) {
            sys::error_code ec;
            asio_utp::socket s(exec);

            auto cancel_con = yield.cancel_slot([&] { s.close(); });

            s.bind(*_udp_multiplexer, ec);
            assert(!ec);
            auto r = s.async_accept(yield);
            if (!r) {
                LOG_ERROR(yield, " UtpOuiServiceServer: failed to accept, will retry in 5s;"
                               , " lep=", s.local_endpoint(), " ec=", r.error());
                async_sleep(5s, yield);
                continue;
            }

            auto ep = util::str("uTP/", s.remote_endpoint());
            r = _accept_queue.async_send(sys::error_code(), {std::move(s), std::move(ep)}, yield);
            if (!r) break;
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
    return std::move(*s);
}

UtpOuiServiceClient::UtpOuiServiceClient( asio::any_io_executor ex
                                        , asio_utp::udp_multiplexer m
                                        , asio::ip::udp::endpoint endpoint):
    _ex(std::move(ex)),
    _remote_endpoint(endpoint),
    _udp_multiplexer(std::move(m))
{
}

sys::error_code UtpOuiServiceClient::start(Async) {
    return sys::error_code();
}

std::expected<GenericStream, sys::error_code>
UtpOuiServiceClient::connect(Async yield)
{
    using namespace chrono_literals;

    sys::error_code ec;
    asio_utp::socket socket(yield.get_executor());

    static const chrono::seconds retry_timeout[] = { 4s , 8s , 16s };

    for (int i = 0; i != sizeof(retry_timeout)/sizeof(*retry_timeout); ++i) {
        ec = sys::error_code();

        socket = asio_utp::socket(_ex);
        socket.bind(_udp_multiplexer, ec);
        assert(!ec);

        auto result = timeout(
            retry_timeout[i],
            [&](auto yield) { return socket.async_connect(_remote_endpoint, yield); },
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
