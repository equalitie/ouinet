#pragma once

#include <boost/intrusive/list.hpp>
#include "condition_variable.h"
#include "../or_throw.h"

namespace ouinet {

/*
 * Implement a simple queue-like scheduler. It allows coroutines to be
 * scheduled in a FIFO queue and run `max_running_jobs` of them concurrently.
 *
 * Usage:
 *
 * const size_t max_running_jobs = 5;
 *
 * Scheduler s(ios, max_running_jobs);
 *
 * for (size_t i = 0; i < 100; ++i) {
 *     spawn(ios, [&s] (asio::yield_context yield) {
 *         system::error_code ec;
 *
 *         // This blocks if we have more than `max_running_jobs` number of
 *         // Slots instantiated already. When a `slot` is destroyed (by
 *         // running out of scope) from some other coroutine, the next
 *         // coroutine in queue blocking on `wait_for_slow` will resume.
 *
 *         Slot slot = s.wait_for_slot(yield[ec])
 *         if (ec) return;
 *
 *         // Do your async tasks here with the guarantee that there will be
 *         // at most max_running_jobs running of them at any given time.
 *
 *         // ...
 *     });
 * }
 */

class Scheduler {
private:
    using ListHook = boost::intrusive::list_base_hook<>;

    template<class T>
    using List = boost::intrusive::list<T>;

    struct Waiter : public ListHook {
        Waiter(asio::io_service& ios) : cv(ios) {}
        ConditionVariable cv;
    };

public:

    class Slot : public ListHook {
    private:
        using OnExit = std::function<void()>;

    public:
        Slot(const Slot&) = delete;

        Slot(Slot&& o) : scheduler(o.scheduler) {
            swap_nodes(o);
            o.scheduler = nullptr;
        }

        Slot& operator=(Slot&& o) {
            swap_nodes(o);
            scheduler = o.scheduler;
            o.scheduler = nullptr;
            return *this;
        }

        ~Slot();

    private:
        friend class Scheduler;
        Slot() {}
        Slot(Scheduler* s) : scheduler(s) {}

    private:
        Scheduler* scheduler = nullptr;
    };

public:
    Scheduler(asio::io_service& ios, size_t max_running_jobs = 1);

    Slot wait_for_slot(asio::yield_context yield);

    size_t max_running_jobs() const { return _max_running_jobs; }

    ~Scheduler();

private:
    void release_slot(Slot&);

private:
    asio::io_service& _ios;
    size_t _max_running_jobs;
    List<Slot> _slots;
    List<Waiter> _waiters;
};

inline
Scheduler::Scheduler(asio::io_service& ios, size_t max_running_jobs)
    : _ios(ios)
    , _max_running_jobs(max_running_jobs)
{}

inline
Scheduler::Slot Scheduler::wait_for_slot(asio::yield_context yield)
{
    if (_slots.size() >= _max_running_jobs) {
        Waiter waiter(_ios);

        _waiters.push_back(waiter);

        sys::error_code ec;
        waiter.cv.wait(yield[ec]);

        if (!waiter.is_linked()) {
            // `this` scheduler has been destroyed
            if (!ec) ec = asio::error::operation_aborted;
        }
        else {
            _waiters.erase(_waiters.iterator_to(waiter));
        }

        if (ec) {
            return or_throw(yield, ec, Slot());
        }
    }

    Slot slot(this);
    _slots.push_back(slot);
    return slot;
}

inline
Scheduler::Slot::~Slot() {
    if (!scheduler) {
        // Was either moved from or the scheduler has been destroyed.
        return;
    }

    auto& slots = scheduler->_slots;
    slots.erase(slots.iterator_to(*this));

    auto& waiters = scheduler->_waiters;

    if (waiters.empty()) return;
    Waiter& next = waiters.front();

    next.cv.notify();
}

inline
void Scheduler::release_slot(Slot& slot)
{
    _slots.erase(_slots.iterator_to(slot));
    if (_waiters.empty()) return;
    Waiter& next = _waiters.front();
    next.cv.notify();
}

inline
Scheduler::~Scheduler()
{
    for (auto& slot : _slots) {
        slot.scheduler = nullptr;
    }

    for (auto& waiter : _waiters) {
        waiter.cv.notify(asio::error::operation_aborted);
    }
}

} // namespace
