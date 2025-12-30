#include "tls.h"
#include "../or_throw.h"
#include "../ssl/util.h"
#include "../util/watch_dog.h"
#include "../util/handler_tracker.h"
#include "../async_sleep.h"
#include "../util/ssl_stream.h"
#include <iostream>

namespace ouinet {
namespace ouiservice {

using namespace std;

void TlsOuiServiceServer::start_listen(asio::yield_context yield) /* override */
{
    _base->start_listen(yield);

    TRACK_SPAWN(_ex, ([&] (asio::yield_context yield) {
            using namespace chrono_literals;

            Cancel cancel(_cancel);

            while (!cancel) {
                sys::error_code ec;

                auto base_con = _base->accept(yield[ec]);

                if (cancel || ec == asio::error::operation_aborted) break;

                if (ec) {
                    async_sleep(_ex, 100ms, cancel, yield);
                    if (cancel) break;
                    continue;
                }

                auto tls_con = SslStream(move(base_con), _ssl_context);

                // Spawn a new coroutine to avoid blocking accept of the next
                // socket.
                TRACK_SPAWN(_ex, ([ tls_con = move(tls_con)
                                  , cancel = move(cancel)
                                  , &q = _accept_queue
                                  , ex = _ex
                                  ] (auto yield) mutable {
                    sys::error_code ec;

                    auto wd = watch_dog( ex, 10s
                                       , [&] {
                                             tls_con->next_layer().close();
                                         });

                    tls_con->async_handshake( asio::ssl::stream_base::server
                                            , yield[ec]);
                    ec = compute_error_code(ec, cancel, wd);
                    if (ec) return;  // do not propagate error

                    q.async_push( GenericStream(move(tls_con))
                                , cancel
                                , yield[ec]);  // do not propagate error
                }));
            }
        }));
};

void TlsOuiServiceServer::stop_listen() /* override */
{
    _cancel();

    while (!_accept_queue.empty()) {
        auto c = move(_accept_queue.back());
        _accept_queue.pop();
        c.close();
    }

    _base->stop_listen();
};

GenericStream TlsOuiServiceServer::accept(asio::yield_context yield)
{
    sys::error_code ec;
    auto s = _accept_queue.async_pop(_cancel, yield[ec]);
    return or_throw(yield, ec, move(s));
}

TlsOuiServiceServer::~TlsOuiServiceServer()
{
    _cancel();
}

GenericStream
TlsOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    sys::error_code ec;

    auto connection = _base->connect(yield[ec], cancel);

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    // This also gets a configured shutter.
    // The certificate host name is not checked since
    // it may be missing (e.g. IP address) or meaningless (e.g. I2P identifier).
    return ssl::util::client_handshake( std::move(connection)
                                      , _ssl_context, ""
                                      , cancel
                                      , yield);
}


} // ouiservice namespace
} // ouinet namespace
