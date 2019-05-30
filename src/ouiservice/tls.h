#pragma once

#include <boost/asio/ssl.hpp>

#include "../ouiservice.h"
#include "../util/signal.h"
#include "../util/async_queue.h"

namespace ouinet {
namespace ouiservice {

// Wraps TLS over an existing service.
class TlsOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    using BaseServicePtr = std::unique_ptr<OuiServiceImplementationServer>;

    TlsOuiServiceServer( asio::io_service& ios
                       , BaseServicePtr base
                       , asio::ssl::context& context)
        : _ios(ios)
        , _base(std::move(base))
        , _ssl_context(context)
        , _accept_queue(_ios /*, TODO: max size? */)
    {};

    void start_listen(asio::yield_context) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    ~TlsOuiServiceServer();

    private:
    asio::io_service& _ios;
    BaseServicePtr _base;
    asio::ssl::context& _ssl_context;
    Cancel _cancel;
    util::AsyncQueue<GenericStream> _accept_queue;
};

class TlsOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    using BaseServicePtr = std::unique_ptr<OuiServiceImplementationClient>;

    public:
    TlsOuiServiceClient(BaseServicePtr base_, asio::ssl::context& context):
        _base(std::move(base_)), _ssl_context(context)
    {};

    void start(asio::yield_context yield) override {
        _base->start(yield);
    }

    void stop() override {
        _base->stop();
    }

    GenericStream connect(asio::yield_context, Cancel&) override;

    private:
    BaseServicePtr _base;
    asio::ssl::context& _ssl_context;
};

} // ouiservice namespace
} // ouinet namespace
