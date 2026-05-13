#pragma once
#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

class WeakOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    using BaseServicePtr = std::weak_ptr<OuiServiceImplementationClient>;

    public:
    WeakOuiServiceClient(BaseServicePtr base_):
        _base(std::move(base_))
    {};

    [[nodiscard]]
    sys::error_code start(Async yield) override {
        auto ptr = _base.lock();
        if (!ptr) return asio::error::bad_descriptor;
        return ptr->start(yield);
    }

    void stop() override {
        if (auto ptr = _base.lock()) ptr->stop();
    }

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async yield) override {
        auto ptr = _base.lock();

        if (!ptr) {
            return std::unexpected(asio::error::bad_descriptor);
        }

        return ptr->connect(yield);
    }

    private:
    BaseServicePtr _base;
};


} // ouiservice namespace
} // ouinet namespace
