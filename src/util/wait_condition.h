#pragma once

#include <memory>

#include "signal.h"
#include "intrusive_list.h"
#include "yield.h"
#include <boost/asio/any_completion_handler.hpp>

namespace ouinet {

/*
 * Waits for all members of a set of coroutines to finish a task.
 *
 * Usage:
 *
 * WaitCondition wait_condition(executor);
 *
 * spawn(executor, [lock = wait_condition.lock()](auto yield) {
 *     do_something(yield);
 * }
 * spawn(executor, [lock = wait_condition.lock()](auto yield) {
 *     do_something(yield);
 *     // This is unnecessary, for release() is called on lock destructor
 *     lock.release();
 * }
 *
 * // Returns when both of the above coroutines have called release()
 * // or the destructor on their lock
 * wait_condition.wait(yield);
 */

class WaitCondition {
private:
    struct WaitState;

public:
    class Lock {
        friend class WaitState;
        friend class WaitCondition;

    public:
        Lock(std::shared_ptr<WaitState> wait_state);
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        Lock(Lock&&);
        Lock& operator=(Lock&&);

        ~Lock();

        void release();

    private:
        util::intrusive::list_hook hook;
        mutable std::shared_ptr<WaitState> _wait_state;
    };

private:
    struct Waiter : std::enable_shared_from_this<Waiter> {
        util::intrusive::list_hook hook;
        asio::any_completion_handler<void(sys::error_code)> handler;
        bool aborted = false;

        void complete(auto& exec) {
            // Make sure this waiter won't be completed again before the below
            // `post` finishes.
            hook.unlink();

            asio::post(exec, [self = std::move(shared_from_this())] () mutable {
                    sys::error_code ec;
                    if (self->aborted) {
                        ec = asio::error::operation_aborted;
                    }
                    self->handler(ec);
                    // The `handles` holds shared ptr to this waiter. Resetting
                    // it so `this` can be destroyed.
                    self->handler = {};
                }
            );
        }
    };

    struct WaitState {
        asio::any_io_executor exec;
        util::intrusive::list<Lock, &Lock::hook> locks;
        util::intrusive::list<Waiter, &Waiter::hook> waiters;

        WaitState(auto exec) : exec(std::move(exec)) {}
    };

public:
    WaitCondition(asio::any_io_executor);
    WaitCondition(boost::asio::io_context&);
    WaitCondition(const WaitCondition&) = delete;
    WaitCondition& operator=(const WaitCondition&) = delete;

    template<class CompletionToken> auto wait(CompletionToken);
    template<class CompletionToken> auto wait(Cancel&, CompletionToken);

    Lock lock();

    size_t size() const {
        if (!_wait_state) return 0;
        return _wait_state->locks.size();
    }

    ~WaitCondition();

private:
    template<class CompletionToken> auto do_wait(Cancel*, CompletionToken);

private:
    asio::any_io_executor _exec;
    std::shared_ptr<WaitState> _wait_state;
};

inline
WaitCondition::Lock::Lock(std::shared_ptr<WaitCondition::WaitState> wait_state):
    _wait_state(std::move(wait_state))
{
    _wait_state->locks.push_back(*this);
}

inline
WaitCondition::Lock::Lock(WaitCondition::Lock&& other)
{
    (*this) = std::move(other);
}

inline
WaitCondition::Lock& WaitCondition::Lock::operator=(WaitCondition::Lock&& other)
{
    hook.swap_nodes(other.hook);
    _wait_state.swap(other._wait_state);
    other.hook.unlink();
    return *this;
}

inline
WaitCondition::Lock::~Lock()
{
    release();
}

inline
void WaitCondition::Lock::release()
{
    if (!_wait_state) {
        return; // moved
    }

    if (!hook.is_linked()) {
        return; // released
    }

    hook.unlink();

    if (_wait_state->locks.empty()) {
        auto& waiters = _wait_state->waiters;
        while (!waiters.empty()) {
            waiters.front().complete(_wait_state->exec);
        }
    }

    _wait_state.reset();
}

inline
WaitCondition::WaitCondition(asio::any_io_executor exec):
    _exec(std::move(exec))
{}

inline
WaitCondition::~WaitCondition() {
    if (!_wait_state) return;

    auto& waiters = _wait_state->waiters;
    while (!waiters.empty()) {
        auto& waiter = waiters.front();
        waiter.aborted = true;
        waiter.complete(_wait_state->exec);
    }
}

inline
WaitCondition::WaitCondition(boost::asio::io_context& ctx):
    _exec(ctx.get_executor())
{}

template<class CompletionToken>
inline
auto WaitCondition::wait(CompletionToken token)
{
    return do_wait(nullptr, std::forward<CompletionToken>(token));
}

template<class CompletionToken>
inline
auto WaitCondition::wait(Cancel& cancel, CompletionToken token)
{
    return do_wait(&cancel, std::forward<CompletionToken>(token));
}

template<class CompletionToken>
inline
auto WaitCondition::do_wait(Cancel* cancel, CompletionToken token)
{
    return boost::asio::async_initiate<CompletionToken, void(sys::error_code)
        >([&] (auto handler) {
            if (!_wait_state || _wait_state->locks.empty()) {
                return handler(sys::error_code{});
            }

            Cancel::Connection cancel_slot;

            auto waiter = std::make_shared<Waiter>();
            _wait_state->waiters.push_back(*waiter);

            if (cancel) {
                cancel_slot = cancel->connect([&] {
                    waiter->aborted = true;
                    waiter->complete(_exec);
                });
            }

            waiter->handler = [
                waiter, // Preserve waiter's lifetime until handler is executed
                handler = std::move(handler),
                cancel_slot = std::move(cancel_slot)
            ] (sys::error_code ec) mutable {
                handler(ec);
            };
        },
        token);
}

inline
WaitCondition::Lock WaitCondition::lock()
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_exec);
    }

    return WaitCondition::Lock(_wait_state);
}

} // ouinet namespace
