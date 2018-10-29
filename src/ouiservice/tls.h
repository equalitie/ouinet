#pragma once

#include <boost/asio/ssl.hpp>

#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

// Wraps TLS over an existing service.
class TlsOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    using BaseServicePtr = std::unique_ptr<OuiServiceImplementationServer>;

    TlsOuiServiceServer( BaseServicePtr base_, asio::ssl::context context):
        base(std::move(base_)), ssl_context(std::move(context))
    {};

    void start_listen(asio::yield_context yield) override {
        base->start_listen(yield);
    };
    void stop_listen() override {
        base->stop_listen();
    };

    GenericStream accept(asio::yield_context yield) override;

    private:
    BaseServicePtr base;
    asio::ssl::context ssl_context;
};

} // ouiservice namespace
} // ouinet namespace
