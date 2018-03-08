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
        bool is_released;
        int remaining_locks;
        bool success;

        WaitState(boost::asio::io_service& ios);
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

        void release(bool success);

    private:
        std::shared_ptr<WaitState> _wait_state;
    };

public:
    SuccessCondition(boost::asio::io_service& ios);
    SuccessCondition(const SuccessCondition&) = delete;
    SuccessCondition& operator=(const SuccessCondition&) = delete;

    bool wait_for_success(boost::asio::yield_context yield);

    Lock lock();

private:
    boost::asio::io_service& _ios;
    std::shared_ptr<WaitState> _wait_state;
};

inline
SuccessCondition::WaitState::WaitState(boost::asio::io_service& ios):
    condition(ios),
    is_released(false),
    remaining_locks(0),
    success(false)
{}

inline
SuccessCondition::Lock::Lock(const std::shared_ptr<WaitState>& wait_state):
    _wait_state(wait_state)
{
    assert(!_wait_state->is_released);
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
void SuccessCondition::Lock::release(bool success)
{
    if (!_wait_state) {
        return;
    }

    _wait_state->remaining_locks--;
    if (!_wait_state->is_released) {
        assert(!_wait_state->success);
        if (success || _wait_state->remaining_locks == 0) {
            _wait_state->success = success;
            _wait_state->is_released = true;
            _wait_state->condition.notify_one();
        }
    }
    _wait_state.reset();
}

inline
SuccessCondition::SuccessCondition(boost::asio::io_service& ios):
    _ios(ios)
{}

inline
bool SuccessCondition::wait_for_success(boost::asio::yield_context yield)
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_ios);
    }

    std::shared_ptr<WaitState> wait_state = std::move(_wait_state);
    if (!wait_state->is_released) {
        wait_state->condition.wait(yield);
    }
    return wait_state->success;
}

inline
SuccessCondition::Lock SuccessCondition::lock()
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_ios);
    }

    return SuccessCondition::Lock(_wait_state);
}

} // ouinet namespace
