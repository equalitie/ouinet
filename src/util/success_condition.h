#pragma once

#include <memory>

#include "condition_variable.h"

namespace ouinet {

/*
 * Waits for either one of a set of coroutines to finish a task successfully,
 * or all of them to finish the task unsuccessfully.
 *
 * Usage:
 *
 * SuccessCondition success_condition(ios);
 *
 * spawn(ios, [lock = success_condition.lock()](auto yield) {
 *     if (!do_something(yield)) {
 *         // lock destructor implies unsuccessful completion
 *         return;
 *     }
 *     // operation finished successfully
 *     lock.release(true);
 * });
 * spawn(ios, [lock = success_condition.lock()](auto yield) {
 *     if (!do_something(yield)) {
 *         // lock destructor implies unsuccessful completion
 *         return;
 *     }
 *     // operation finished successfully
 *     lock.release(true);
 * });
 *
 * // Returns when one of the two coroutines has called release(true),
 * // OR all of them have failed.
 * success_condition.wait_for_success(yield);
 */

class SuccessCondition {
private:
    struct WaitState {
        ConditionVariable condition;
        int remaining_locks;
        bool success;

        bool blocked() {
            return remaining_locks > 0 && !success;
        }

        WaitState(const boost::asio::executor&);
    };

public:
    class Lock {
    public:
        Lock(const std::shared_ptr<WaitState>& wait_state);
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        Lock(Lock&&);
        Lock& operator=(Lock&&);

        ~Lock();

        void release(bool success) const;

    private:
        mutable std::shared_ptr<WaitState> _wait_state;
    };

public:
    SuccessCondition(const boost::asio::executor&);
    SuccessCondition(const SuccessCondition&) = delete;
    SuccessCondition& operator=(const SuccessCondition&) = delete;

    bool wait_for_success(boost::asio::yield_context yield);

    Lock lock();

    void cancel();
    bool cancelled() {
        return _cancelled;
    }

private:
    boost::asio::executor _exec;
    std::shared_ptr<WaitState> _wait_state;
    Signal<void()> _cancel_signal;
    bool _cancelled;
};



inline
SuccessCondition::WaitState::WaitState(const boost::asio::executor& exec):
    condition(exec),
    remaining_locks(0),
    success(false)
{}

inline
SuccessCondition::Lock::Lock(const std::shared_ptr<SuccessCondition::WaitState>& wait_state):
    _wait_state(wait_state)
{
    _wait_state->remaining_locks++;
}

inline
SuccessCondition::Lock::Lock(SuccessCondition::Lock&& other)
{
    (*this) = std::move(other);
}

inline
SuccessCondition::Lock& SuccessCondition::Lock::operator=(SuccessCondition::Lock&& other)
{
    release(false);
    _wait_state = other._wait_state;
    other._wait_state.reset();
    return *this;
}

inline
SuccessCondition::Lock::~Lock()
{
    release(false);
}

inline
void SuccessCondition::Lock::release(bool success) const
{
    if (!_wait_state) {
        return;
    }

    _wait_state->remaining_locks--;
    if (success) {
        _wait_state->success = true;
    }
    if (!_wait_state->blocked()) {
        _wait_state->condition.notify();
    }
    _wait_state.reset();
}

inline
SuccessCondition::SuccessCondition(const boost::asio::executor& exec):
    _exec(exec),
    _cancelled(false)
{}

inline
bool SuccessCondition::wait_for_success(boost::asio::yield_context yield)
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_exec);
    }

    std::shared_ptr<WaitState> wait_state = std::move(_wait_state);
    if (wait_state->blocked()) {
        auto cancel_slot = _cancel_signal.connect([&wait_state] {
            wait_state->condition.notify();
        });
        wait_state->condition.wait(yield);
    }
    return wait_state->success;
}

inline
SuccessCondition::Lock SuccessCondition::lock()
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_exec);
    }

    return SuccessCondition::Lock(_wait_state);
}

inline
void SuccessCondition::cancel()
{
    _cancelled = true;
    _cancel_signal();
}

} // ouinet namespace
