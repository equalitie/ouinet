#pragma once

#include "../ouiservice.h"
#include "../util/signal.h"
#include "../util/async_queue.h"

namespace ouinet {
namespace ouiservice {

// Wraps HTTP CONNECT proxy over an existing service.
class ConnectProxyOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    using BaseServicePtr = std::unique_ptr<OuiServiceImplementationClient>;

    public:
    ConnectProxyOuiServiceClient(BaseServicePtr base_):
        _base(std::move(base_))
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
};

} // ouiservice namespace
} // ouinet namespace
