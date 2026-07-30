#pragma once

#include "../ouiservice.h"
#include "../util/async_queue.h"

namespace ouinet {
namespace ouiservice {

// Wraps HTTP CONNECT proxy over an existing service.
class ConnectProxyOuiServiceClient : public OuiServiceClient
{
public:
    using BaseServicePtr = std::unique_ptr<OuiServiceClient>;

public:
    ConnectProxyOuiServiceClient(BaseServicePtr base_):
        _base(std::move(base_))
    {};

    [[nodiscard]]
    sys::error_code start(Async yield) override {
        return _base->start(yield);
    }

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async) override;

private:
    BaseServicePtr _base;
};

} // ouiservice namespace
} // ouinet namespace
