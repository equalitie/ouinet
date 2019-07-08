#pragma once

#include <memory>

#include "signal.h"
#include "condition_variable.h"
#include "../or_throw.h"

namespace ouinet {

/*
 * Waits for all members of a set of coroutines to finish a task.
 *
 * Usage:
 *
 * WaitCondition wait_condition(ios);
 *
 * spawn(ios, [lock = wait_condition.lock()](auto yield) {
 *     do_something(yield);
 * }
 * spawn(ios, [lock = wait_condition.lock()](auto yield) {
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
    struct WaitState {
        ConditionVariable condition;
        int remaining_locks;

        bool blocked() {
            return remaining_locks > 0;
        }

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

        void release() const;

    private:
        mutable std::shared_ptr<WaitState> _wait_state;
    };

public:
    WaitCondition(boost::asio::io_service& ios);
    WaitCondition(const WaitCondition&) = delete;
    WaitCondition& operator=(const WaitCondition&) = delete;

    void wait(boost::asio::yield_context yield);
    void wait(Cancel&, boost::asio::yield_context yield);

    Lock lock();

    size_t size() const {
        if (!_wait_state) return 0;
        return _wait_state->remaining_locks;
    }

private:
    void do_wait(Cancel*, boost::asio::yield_context yield);

private:
    boost::asio::io_service& _ios;
    std::shared_ptr<WaitState> _wait_state;
};



inline
WaitCondition::WaitState::WaitState(boost::asio::io_service& ios):
    condition(ios),
    remaining_locks(0)
{}

inline
WaitCondition::Lock::Lock(const std::shared_ptr<WaitCondition::WaitState>& wait_state):
    _wait_state(wait_state)
{
    _wait_state->remaining_locks++;
}

inline
WaitCondition::Lock::Lock(WaitCondition::Lock&& other)
{
    (*this) = std::move(other);
}

inline
WaitCondition::Lock& WaitCondition::Lock::operator=(WaitCondition::Lock&& other)
{
    release();
    _wait_state = other._wait_state;
    other._wait_state.reset();
    return *this;
}

inline
WaitCondition::Lock::~Lock()
{
    release();
}

inline
void WaitCondition::Lock::release() const
{
    if (!_wait_state) {
        return;
    }

    if (!_wait_state->blocked()) {
        _wait_state.reset();
        return;
    }

    _wait_state->remaining_locks--;

    if (!_wait_state->blocked()) {
        _wait_state->condition.notify();
    }

    _wait_state.reset();
}

inline
WaitCondition::WaitCondition(boost::asio::io_service& ios):
    _ios(ios)
{}

inline
void WaitCondition::wait(boost::asio::yield_context yield)
{
    do_wait(nullptr, yield);
}

inline
void WaitCondition::wait(Cancel& cancel, boost::asio::yield_context yield)
{
    do_wait(&cancel, yield);
}

inline
void WaitCondition::do_wait(Cancel* cancel, boost::asio::yield_context yield)
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_ios);
    }

    std::shared_ptr<WaitState> wait_state = std::move(_wait_state);

    if (wait_state->blocked()) {
        Cancel::Connection con;

        if (cancel) {
            con = cancel->connect([&] {
                if (!wait_state->blocked()) return;
                wait_state->remaining_locks = 0;
                wait_state->condition.notify();
            });
        }

        wait_state->condition.wait(yield);

        if (con) {
            return or_throw(yield, asio::error::operation_aborted);
        }
    }
}

inline
WaitCondition::Lock WaitCondition::lock()
{
    if (!_wait_state) {
        _wait_state = std::make_shared<WaitState>(_ios);
    }

    return WaitCondition::Lock(_wait_state);
}

} // ouinet namespace
