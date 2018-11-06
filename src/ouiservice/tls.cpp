#include "tls.h"
#include "../or_throw.h"
#include "../ssl/util.h"

namespace ouinet {
namespace ouiservice {

GenericStream TlsOuiServiceServer::accept(asio::yield_context yield)
{
    using SslStream = asio::ssl::stream<GenericStream>;
    sys::error_code ec;

    std::unique_ptr<SslStream> tls_sock;

    while (!tls_sock) {
        auto base_con = base->accept(yield[ec]);
        if (ec) {  // hard error, propagate
            return or_throw<GenericStream>(yield, ec);
        }

        tls_sock = std::make_unique<SslStream>(std::move(base_con), ssl_context);
        tls_sock->async_handshake(asio::ssl::stream_base::server, yield[ec]);
        if (ec) {  // soft error, try again
            tls_sock.release();
            ec = sys::error_code();
        }
    }

    static const auto tls_shutter = [](SslStream& s) {
        // Just close the underlying connection
        // (TLS has no message exchange for shutdown).
        s.next_layer().close();
    };

    return GenericStream(std::move(tls_sock), std::move(tls_shutter));
}

OuiServiceImplementationClient::ConnectInfo
TlsOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    sys::error_code ec;

    auto base_coninfo = base->connect(yield[ec], cancel);
    if (ec) {
        return or_throw<ConnectInfo>(yield, ec);
    }

    // This also gets a configured shutter.
    // The certificate host name is not checked since
    // it may be missing (e.g. IP address) or meaningless (e.g. I2P identifier).
    auto tls_con = ssl::util::client_handshake( std::move(base_coninfo.connection)
                                              , ssl_context, ""
                                              , cancel
                                              , yield[ec]);

    ConnectInfo tls_coninfo{std::move(tls_con), base_coninfo.remote_endpoint};
    return or_throw(yield, ec, std::move(tls_coninfo));
}


} // ouiservice namespace
} // ouinet namespace
