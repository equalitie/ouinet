#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/intrusive/list.hpp>

namespace ouinet {

class ConditionVariable {
    using Sig = void(boost::system::error_code);

    using IntrusiveHook = boost::intrusive::list_base_hook
        <boost::intrusive::link_mode
            <boost::intrusive::auto_unlink>>;

    struct WaitEntry : IntrusiveHook {
        std::function<Sig> handler;
    };

    using IntrusiveList = boost::intrusive::list
        <WaitEntry, boost::intrusive::constant_time_size<false>>;

public:
    ConditionVariable(boost::asio::io_service& ios);

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    ~ConditionVariable();

    asio::io_service& get_io_service() { return _ios; }

    void notify(const boost::system::error_code& ec
                    = boost::system::error_code());

    void wait(boost::asio::yield_context yield);

private:
    boost::asio::io_service& _ios;
    IntrusiveList _on_notify;
};

inline
ConditionVariable::ConditionVariable(boost::asio::io_service& ios)
    : _ios(ios)
{
}

inline
ConditionVariable::~ConditionVariable()
{
    notify(boost::asio::error::operation_aborted);
}

inline
void ConditionVariable::notify(const boost::system::error_code& ec)
{
    while (!_on_notify.empty()) {
        auto& e = _on_notify.front();
        _ios.post([h = std::move(e.handler), ec] () mutable { h(ec); });
        _on_notify.pop_front();
    }
}

inline
void ConditionVariable::wait(boost::asio::yield_context yield)
{
    boost::asio::async_completion<boost::asio::yield_context, Sig> init(yield);
    WaitEntry entry;
    entry.handler = std::move(init.completion_handler);
    _on_notify.push_back(entry);
    return init.result.get();
}

} // ouinet namespace
