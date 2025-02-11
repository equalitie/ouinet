#pragma once

#include <boost/intrusive/list.hpp>
#include <logger.h>
#include "task.h"

namespace ouinet {

class HandlerTracker final {
private:
    using Hook = boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;
    struct Entry : Hook { HandlerTracker* self; };
    using List = boost::intrusive::list<Entry, boost::intrusive::constant_time_size<false>>;

    struct GlobalState;

public:
    static void stopped();

    HandlerTracker(const char* name, bool after_stop = false);

    const char* name() const { return _name; }

    ~HandlerTracker();

private:
    static GlobalState& global_state();

private:
    const char* _name;
    Entry _entry;
};

} // ouinet namespace

#define OUINET_DETAIL_HANDLER_TRACKER_STRINGIFY_(x) #x
#define OUINET_DETAIL_HANDLER_TRACKER_STRINGIFY(x) OUINET_DETAIL_HANDLER_TRACKER_STRINGIFY_(x)
#define OUINET_DETAIL_HANDLER_TRACKER_LOCATION_STRING() \
    __FILE__ ":" OUINET_DETAIL_HANDLER_TRACKER_STRINGIFY(__LINE__)

#define TRACK_HANDLER_AFTER_STOP() \
    HandlerTracker handler_tracker_instance(OUINET_DETAIL_HANDLER_TRACKER_LOCATION_STRING(), true)

#define TRACK_HANDLER() \
    HandlerTracker handler_tracker_instance(OUINET_DETAIL_HANDLER_TRACKER_LOCATION_STRING(), false)

#define TRACK_SPAWN(exec, body, ...)\
    task::spawn_detached(exec, [b = body] (asio::yield_context yield) mutable {\
        TRACK_HANDLER();\
        try {\
            b(yield);\
        } catch (const std::exception& e) {\
            LOG_ERROR("Uncaught exception from coroutine " \
                      OUINET_DETAIL_HANDLER_TRACKER_LOCATION_STRING() ": ", e.what()); \
        }\
    })

#define TRACK_SPAWN_AFTER_STOP(exec, body, ...)\
    task::spawn_detached(exec, [b = body] (asio::yield_context yield) mutable {\
        TRACK_HANDLER_AFTER_STOP();\
        try {\
            b(yield);\
        } catch (const std::exception& e) {\
            LOG_ERROR("Uncaught exception from coroutine " \
                      OUINET_DETAIL_HANDLER_TRACKER_LOCATION_STRING() ": ", e.what()); \
        }\
    })
