#pragma once

#include <boost/asio/ssl.hpp>
#include <boost/asio/experimental/channel.hpp>

#include "../ouiservice.h"
#include "../util/cancel.h"
#include "../util/async.h"

namespace ouinet {

namespace ouiservice {

// Wraps TLS over an existing service.
class TlsOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    using BaseServicePtr = std::unique_ptr<OuiServiceImplementationServer>;

    TlsOuiServiceServer( asio::any_io_executor ex
                       , BaseServicePtr base
                       , asio::ssl::context& context)
        : _ex(std::move(ex))
        , _base(std::move(base))
        , _ssl_context(context)
        , _accept_queue(_ex, 256)
    {};

    [[nodiscard]]
    sys::error_code start_listen(Async) override;
    void stop_listen() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async) override;

    ~TlsOuiServiceServer();

    private:
    AsioExecutor _ex;
    BaseServicePtr _base;
    asio::ssl::context& _ssl_context;
    Cancel _cancel;
    asio::experimental::channel<void(sys::error_code, GenericStream)> _accept_queue;
};

class TlsOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    using BaseServicePtr = std::unique_ptr<OuiServiceImplementationClient>;

    public:
    TlsOuiServiceClient(BaseServicePtr base_, asio::ssl::context& context):
        _base(std::move(base_)), _ssl_context(context)
    {};

    sys::error_code start(Async yield) override {
        return _base->start(yield);
    }

    void stop() override {
        _base->stop();
    }

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async) override;

    private:
    BaseServicePtr _base;
    asio::ssl::context& _ssl_context;
};

} // ouiservice namespace
} // ouinet namespace
