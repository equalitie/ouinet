#pragma once

#include <boost/intrusive/list.hpp>
#include "condition_variable.h"
#include "../or_throw.h"
#include "signal.h"

namespace ouinet {

/*
 * Implement a simple queue-like scheduler. It allows coroutines to be
 * scheduled in a FIFO queue and run `max_running_jobs` of them concurrently.
 *
 * Usage:
 *
 * const size_t max_running_jobs = 5;
 *
 * Scheduler s(exec, max_running_jobs);
 *
 * for (size_t i = 0; i < 100; ++i) {
 *     spawn(exec, [&s] (asio::yield_context yield) {
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
        Waiter(const asio::executor& exec) : cv(exec) {}
        ConditionVariable cv;
    };

public:

    class Slot : public ListHook {
    private:
        using OnExit = std::function<void()>;

    public:
        Slot() : scheduler(nullptr) {}

        Slot(const Slot&) = delete;

        Slot(Slot&& o) : scheduler(o.scheduler) {
            swap_nodes(o);
            o.scheduler = nullptr;
        }

        Slot& operator=(Slot&& o) {
            if (scheduler) scheduler->release_slot(*this);

            swap_nodes(o);
            scheduler = o.scheduler;
            o.scheduler = nullptr;
            return *this;
        }

        ~Slot();

    private:
        friend class Scheduler;
        Slot(Scheduler* s) : scheduler(s) {}

    private:
        Scheduler* scheduler = nullptr;
    };

public:
    Scheduler(const asio::executor&, size_t max_running_jobs = 1);
    Scheduler(asio::io_context&, size_t max_running_jobs = 1);

    Slot wait_for_slot(asio::yield_context yield);
    Slot wait_for_slot(Cancel&, asio::yield_context yield);
    Slot get_slot() {
        Slot slot(this);
        _slots.push_back(slot);
        return slot;
    }

    size_t max_running_jobs() const { return _max_running_jobs; }

    size_t slot_count() const { return _slots.size(); }
    size_t waiter_count() const { return _waiters.size(); }

    ~Scheduler();

private:
    void release_slot(Slot&);

private:
    asio::executor _exec;
    size_t _max_running_jobs;
    List<Slot> _slots;
    List<Waiter> _waiters;
};

inline
Scheduler::Scheduler(const asio::executor& exec, size_t max_running_jobs)
    : _exec(exec)
    , _max_running_jobs(max_running_jobs)
{}

inline
Scheduler::Scheduler(asio::io_context& ctx, size_t max_running_jobs)
    : _exec(ctx.get_executor())
    , _max_running_jobs(max_running_jobs)
{}

inline
Scheduler::Slot Scheduler::wait_for_slot(asio::yield_context yield)
{
    Cancel unused_cancel;
    return wait_for_slot(unused_cancel, yield);
}

inline
Scheduler::Slot Scheduler::wait_for_slot( Cancel& cancel
                                        , asio::yield_context yield)
{
    while (_slots.size() >= _max_running_jobs) {
        Waiter waiter(_exec);

        _waiters.push_back(waiter);

        sys::error_code ec;

        {
            auto slot = cancel.connect([&] {
                waiter.cv.notify(asio::error::operation_aborted);
            });

            waiter.cv.wait(yield[ec]);
        }

        if (cancel) ec = asio::error::operation_aborted;

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
    if (scheduler) scheduler->release_slot(*this);
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
