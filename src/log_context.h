#pragma once

#include "logger.h"
#include "namespaces.h"
#include "util/str.h"

#include <chrono>
#include <functional>
#include <memory>

#include <boost/asio/executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/optional.hpp>

namespace ouinet {

class log_context {

public:
    using log_level = log_level_t;

private:
    struct label {
        std::shared_ptr<label> parent;
        std::string stored_description;
        boost::string_view description;
        std::chrono::steady_clock::time_point creation_time;
        bool log_relative_time;

        label(const std::shared_ptr<label>& parent_, const std::string& description_);
        label(const std::shared_ptr<label>& parent_, std::string&& description_);
        label(const std::shared_ptr<label>& parent_, boost::string_view description_);
    };

    class tracker : public std::enable_shared_from_this<tracker> {
        asio::executor _executor;
        Logger* _sink;
        std::shared_ptr<log_context::label> _label;
        bool _log_destruction;
        bool _active_watchdog;
        std::chrono::steady_clock::duration _watchdog_period;
        asio::steady_timer _watchdog_timer;

    private:
        void arm_watchdog();

    public:
        tracker(asio::executor executor, Logger* sink, const std::shared_ptr<label>& root_label);
        tracker(const tracker& parent, const std::shared_ptr<label>& child_label);
        ~tracker();

        const std::shared_ptr<log_context::label>& label()
            { return _label; }

        void start_watchdog(std::chrono::steady_clock::duration duration);
        void log_relative_time(bool enable)
            { _label->log_relative_time = enable; }
        void log_destruction(bool enable)
            { _log_destruction = enable; }

        void log(log_level level, boost::string_view message);
        bool would_log(log_level level) const;
    };

private:
    log_context(const std::shared_ptr<tracker>& tracker):
        _tracker(tracker)
    {}

public:
    log_context();
    log_context(asio::executor executor, Logger* sink);

    log_context(const log_context& other) = default;
    log_context(log_context&& other) = default;
    log_context& operator=(const log_context& other) = default;
    log_context& operator=(log_context&& other) = default;

    /*
     * A tagged log context just appends an additional label
     * to the prefix of logged messages.
     */
    log_context tag(const std::string& description);
    log_context tag(std::string&& description);
    log_context tag(boost::string_view description);
    log_context tag(const char* description);

    /*
     * A tracking log context measures time relative to job start,
     * runs a watchdog timer to signal long-running jobs,
     * and logs start and finish of the job.
     */
    template<class Description>
    log_context track(
        Description&& description,
        boost::optional<std::chrono::steady_clock::duration> duration
            = std::chrono::steady_clock::duration(std::chrono::seconds(30))
    ) {
        return tag(std::forward<Description>(description))
            .track_lifetime(duration);
    }

    static uint64_t make_id();

    log_context track_lifetime(
        boost::optional<std::chrono::steady_clock::duration> duration
            = std::chrono::steady_clock::duration(std::chrono::seconds(30))
    );
    log_context start_watchdog(std::chrono::steady_clock::duration duration = std::chrono::seconds(30))
        { _tracker->start_watchdog(duration); return *this; }
    log_context log_time(bool enable)
        { _tracker->log_relative_time(enable); return *this; }



    bool would_log(log_level level) const
        { return _tracker->would_log(level); }

    template<class... Args> bool log(log_level level, Args&&... args)
    {
        if (!would_log(level))
            return false;
        _tracker->log(level, boost::string_view(util::str(std::forward<Args>(args)...)));
        return true;
    }
    bool log(log_level level, boost::string_view message)
        { if (!would_log(level)) return false; _tracker->log(level, message); return true; }
    bool log(log_level level, std::function<std::string()> message)
        { if (!would_log(level)) return false; _tracker->log(level, message()); return true; }

#define MAKE_CONVENIENCE_LOG(name, level) \
    template<class... Args> bool name(Args&&... args) \
        { return log(level, std::forward<Args>(args)...); } \
    bool name(boost::string_view message) \
        { return log(level, message); } \
    bool name(std::function<std::string()> message) \
        { return log(level, message); }

    MAKE_CONVENIENCE_LOG(silly,   SILLY)
    MAKE_CONVENIENCE_LOG(debug,   DEBUG)
    MAKE_CONVENIENCE_LOG(verbose, VERBOSE)
    MAKE_CONVENIENCE_LOG(info,    INFO)
    MAKE_CONVENIENCE_LOG(warn,    WARN)
    MAKE_CONVENIENCE_LOG(error,   ERROR)
    MAKE_CONVENIENCE_LOG(abort,   ABORT)
#undef MAKE_CONVENIENCE_LOG

private:
    std::shared_ptr<tracker> _tracker;
};

} // ouinet namespace
