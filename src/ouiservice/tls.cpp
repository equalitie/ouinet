#include "tls.h"
#include "../or_throw.h"
#include "../ssl/util.h"
#include "../util/watch_dog.h"
#include "../task.h"
#include "../async_sleep.h"
#include "../util/ssl_stream.h"
#include <iostream>

namespace ouinet {
namespace ouiservice {

sys::error_code TlsOuiServiceServer::start_listen(Async yield) /* override */
{
    sys::error_code ec = _base->start_listen(yield);
    if (ec) return ec;

    yield.spawn(_cancel, [this] (Async yield) {
            using namespace std::chrono_literals;

            while (true) {
                auto base_con = _base->accept(yield);

                if (!base_con.has_value()) {
                    async_sleep(100ms, yield);
                    continue;
                }

                auto tls_con = SslStream(std::move(*base_con), _ssl_context);

                // Spawn a new coroutine to avoid blocking accept of the next
                // socket.
                yield.spawn([ tls_con = std::move(tls_con)
                            , &q = _accept_queue
                            , ex = _ex
                            ] (Async yield) mutable {

                    {
                        auto wd = watch_dog( ex, 10s
                                           , [&] { tls_con->next_layer().close(); });

                        auto r = tls_con->async_handshake( asio::ssl::stream_base::server, yield);

                        if (!wd.is_running()) return;
                        if (!r) return; // do not propagate error
                    }

                    auto r = q.async_send({}, GenericStream(std::move(tls_con)), yield);
                    if (!r) return; // do not propagate error
                });
            }
        });

    return {};
};

void TlsOuiServiceServer::stop_listen()
{
    _cancel();

    while (_accept_queue.try_receive([] (sys::error_code ec, GenericStream s) { s.close(); })) {}

    _base->stop_listen();
};

std::expected<GenericStream, sys::error_code> TlsOuiServiceServer::accept(Async yield)
{
    auto s = _accept_queue.async_receive(yield);
    if (!s.has_value()) return std::unexpected(s.error());
    return std::move(*s);
}

TlsOuiServiceServer::~TlsOuiServiceServer()
{
    _cancel();
}

std::expected<GenericStream, sys::error_code>
TlsOuiServiceClient::connect(Async yield)
{
    auto connection = _base->connect(yield);

    if (!connection.has_value()) {
        return std::unexpected(connection.error());
    }

    // This also gets a configured shutter.
    // The certificate host name is not checked since
    // it may be missing (e.g. IP address) or meaningless (e.g. I2P identifier).
    return ssl::util::client_handshake( std::move(*connection)
                                      , _ssl_context, ""
                                      , yield);
}


} // ouiservice namespace
} // ouinet namespace
