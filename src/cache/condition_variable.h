#pragma once

#include "../namespaces.h"

namespace ouinet {

class ConditionVariable {
public:
    ConditionVariable(asio::io_service&);

    void notify_one();
    void wait(asio::yield_context);

    ~ConditionVariable();

private:
    asio::io_service& _ios;
    std::function<void(sys::error_code)> _on_notify;
};

inline
ConditionVariable::ConditionVariable(asio::io_service& ios)
    : _ios(ios)
{}

inline
void ConditionVariable::notify_one()
{
    if (!_on_notify) return;

    _ios.post([h = std::move(_on_notify)] {
            h(sys::error_code());
        });
}

inline
void ConditionVariable::wait(asio::yield_context yield)
{
    assert(!_on_notify && "Only single consumer at a time");

    using Handler = asio::handler_type< asio::yield_context
                                      , void(sys::error_code)>::type;

    Handler handler(yield);
    asio::async_result<Handler> result(handler);

    _on_notify = std::move(handler);

    return result.get();
}

inline
ConditionVariable::~ConditionVariable()
{
    if (!_on_notify) return;

    _ios.post([h = std::move(_on_notify)] {
            h(asio::error::operation_aborted);
        });
}

} // namespace
