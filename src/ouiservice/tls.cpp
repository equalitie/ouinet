#include "tls.h"
#include "../or_throw.h"
#include "../ssl/util.h"
#include "../async_sleep.h"
#include <iostream>

namespace ouinet {
namespace ouiservice {

using namespace std;

void TlsOuiServiceServer::start_listen(asio::yield_context yield) /* override */
{
    using SslStream = asio::ssl::stream<GenericStream>;

    _base->start_listen(yield);

    asio::spawn(_ios, [&] (asio::yield_context yield) {
            using namespace chrono_literals;

            Cancel cancel(_cancel);

            while (!cancel) {
                sys::error_code ec;

                auto base_con = _base->accept(yield[ec]);

                if (cancel || ec == asio::error::operation_aborted) break;

                if (ec) {
                    async_sleep(_ios, 100ms, cancel, yield);
                    if (cancel) break;
                    continue;
                }

                auto tls_sock = make_unique<SslStream>(std::move(base_con), _ssl_context);
                tls_sock->async_handshake(asio::ssl::stream_base::server, yield[ec]);

                if (cancel || ec == asio::error::operation_aborted) break;
                if (ec) continue;

                static const auto shutter = [](SslStream& s) {
                    // Just close the underlying connection
                    // (TLS has no message exchange for shutdown).
                    s.next_layer().close();
                };

                _accept_queue.async_push( GenericStream( move(tls_sock)
                                                       , move(shutter))
                                        , cancel
                                        , yield[ec]);
            }
        });
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
