#pragma once

#include <boost/asio/ssl.hpp>

#include "tcp.h"

namespace ouinet {
namespace ouiservice {

// This only implements listening to TLS connections over TCP,
// while ideally it should work over any stream.
class TlsOuiServiceServer : public TcpOuiServiceServer
{
    public:
    TlsOuiServiceServer( asio::io_service& ios
                       , asio::ip::tcp::endpoint endpoint
                       , asio::ssl::context context):
        TcpOuiServiceServer(ios, endpoint), ssl_context(std::move(context))
    {};

    GenericStream accept(asio::yield_context yield) override;

    private:
    asio::ssl::context ssl_context;
};

} // ouiservice namespace
} // ouinet namespace
