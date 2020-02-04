#pragma once

#include <boost/intrusive/list.hpp>

namespace ouinet {

class CoroTracker final {
private:
    using Hook = boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;
    struct Entry : Hook { CoroTracker* self; };
    using List = boost::intrusive::list<Entry, boost::intrusive::constant_time_size<false>>;

    struct GlobalState;

public:
    static void stopped();

    CoroTracker(const char* name, bool after_stop = false);

    const char* name() const { return _name; }

    ~CoroTracker();

private:
    static GlobalState& global_state();

private:
    const char* _name;
    Entry _entry;
};

} // ouinet namespace

#define OUINET_DETAIL_CORO_TRACKER_STRINGIFY_(x) #x
#define OUINET_DETAIL_CORO_TRACKER_STRINGIFY(x) OUINET_DETAIL_CORO_TRACKER_STRINGIFY_(x)
#define TRACK_COROUTINE(...) \
    CoroTracker coro_tracker_instance(__FILE__ ":" OUINET_DETAIL_CORO_TRACKER_STRINGIFY(__LINE__) __VA_OPT__(,) __VA_ARGS__)

#define TRACK_SPAWN(exec, body, ...)\
    asio::spawn(exec, [b = body] (asio::yield_context yield) mutable {\
        TRACK_COROUTINE();\
        b(yield);\
    } __VA_OPT__(,) __VA_ARGS__)

#define TRACK_SPAWN_AFTER_STOP(exec, body, ...)\
    asio::spawn(exec, [b = body] (asio::yield_context yield) mutable {\
        TRACK_COROUTINE(true);\
        b(yield);\
    } __VA_OPT__(,) __VA_ARGS__)
