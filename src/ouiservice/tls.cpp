#include "tls.h"
#include "../or_throw.h"

namespace ouinet {
namespace ouiservice {

GenericStream TlsOuiServiceServer::accept(asio::yield_context yield)
{
    using SslStream = asio::ssl::stream<GenericStream>;
    sys::error_code ec;

    auto tcp_con = TcpOuiServiceServer::accept(yield[ec]);
    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    auto tls_sock = std::make_unique<SslStream>(std::move(tcp_con), ssl_context);
    tls_sock->async_handshake(asio::ssl::stream_base::server, yield[ec]);
    if (ec) {
        // See <https://github.com/equalitie/ouinet/issues/16>.
        ec = asio::error::connection_aborted;
        return or_throw<GenericStream>(yield, ec);
    }

    static const auto tls_shutter = [](SslStream& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericStream(std::move(tls_sock), std::move(tls_shutter));
}

} // ouiservice namespace
} // ouinet namespace
