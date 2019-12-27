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

    void start(asio::yield_context yield) override {
        auto ptr = _base.lock();
        if (!ptr) return or_throw(yield, asio::error::bad_descriptor);
        ptr->start(yield);
    }

    void stop() override {
        if (auto ptr = _base.lock()) ptr->stop();
    }

    GenericStream connect(asio::yield_context yield, Cancel& cancel) override {
        auto ptr = _base.lock();

        if (!ptr) {
            return or_throw<GenericStream>(yield, asio::error::bad_descriptor);
        }

        return ptr->connect(yield, cancel);
    }

    private:
    BaseServicePtr _base;
};


} // ouiservice namespace
} // ouinet namespace
