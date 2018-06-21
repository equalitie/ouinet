#pragma once

#include <memory>

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

namespace ouinet {

class ConditionVariable {
public:
    ConditionVariable(boost::asio::io_service& ios);
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    ~ConditionVariable();

    void notify();
    void wait(boost::asio::yield_context yield);

private:
    boost::asio::io_service& _ios;
    std::vector<std::function<void(boost::system::error_code)>> _on_notify;
};

inline
ConditionVariable::ConditionVariable(boost::asio::io_service& ios)
    : _ios(ios)
{}

inline
ConditionVariable::~ConditionVariable()
{
    if (!_on_notify.empty()) {
        _ios.post([handlers = std::move(_on_notify)] {
            for (auto& h : handlers) {
                h(boost::asio::error::operation_aborted);
            }
        });
    }
}

inline
void ConditionVariable::notify()
{
    if (!_on_notify.empty()) {
        _ios.post([handlers = std::move(_on_notify)] {
            for (auto& h : handlers) {
                h(boost::system::error_code());
            }
        });
    }
}

inline
void ConditionVariable::wait(boost::asio::yield_context yield)
{
    using Handler = boost::asio::handler_type<boost::asio::yield_context, void(boost::system::error_code)>::type;

    Handler handler(yield);
    boost::asio::async_result<Handler> result(handler);

    _on_notify.push_back(std::move(handler));

    return result.get();
}

} // ouinet namespace
