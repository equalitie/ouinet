#include "log_context.h"
#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>

namespace ouinet {

log_context::label::label(const std::shared_ptr<label>& parent_, const std::string& description_):
    parent(parent_),
    stored_description(description_),
    creation_time(std::chrono::steady_clock::now()),
    log_relative_time(false)
{
    description = boost::string_view(stored_description);
}

log_context::label::label(const std::shared_ptr<label>& parent_, std::string&& description_):
    parent(parent_),
    stored_description(description_),
    creation_time(std::chrono::steady_clock::now()),
    log_relative_time(false)
{
    description = boost::string_view(stored_description);
}

log_context::label::label(const std::shared_ptr<label>& parent_, boost::string_view description_):
    parent(parent_),
    description(description_),
    creation_time(std::chrono::steady_clock::now()),
    log_relative_time(false)
{}



log_context::tracker::tracker(asio::executor executor, Logger* sink, const std::shared_ptr<log_context::label>& root_label):
    _executor(executor),
    _sink(sink),
    _label(root_label),
    _log_destruction(false),
    _active_watchdog(false),
    _watchdog_timer(executor)
{}

log_context::tracker::tracker(const tracker& parent, const std::shared_ptr<log_context::label>& child_label):
    tracker(parent._executor, parent._sink, child_label)
{}

log_context::tracker::~tracker()
{
    if (_active_watchdog) {
        _active_watchdog = false;
        _watchdog_timer.cancel();
    }
    if (_log_destruction) {
        log(VERBOSE, "Finished");
    }
}

void log_context::tracker::arm_watchdog()
{
    _watchdog_timer.expires_after(_watchdog_period);
    _watchdog_timer.async_wait([
        weak_tracker = std::weak_ptr<tracker>(shared_from_this())
    ] (sys::error_code ec) {
        if (ec) {
            return;
        }

        std::shared_ptr<tracker> real_tracker = weak_tracker.lock();
        if (!real_tracker) {
            return;
        }

        std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
        auto time_elapsed = timestamp - real_tracker->_label->creation_time;
        long seconds = std::chrono::duration_cast<std::chrono::seconds>(time_elapsed).count();
        real_tracker->log(WARN, "Still running after " + std::to_string(seconds) + " seconds");

        real_tracker->arm_watchdog();
    });
}

void log_context::tracker::start_watchdog(std::chrono::steady_clock::duration duration)
{
    if (!_executor) {
        return;
    }

    if (_active_watchdog) {
        _watchdog_timer.cancel();
    }

    _active_watchdog = true;
    _watchdog_period = duration;
    arm_watchdog();
}

void log_context::tracker::log(log_level level, boost::string_view message)
{
    if (!_sink) {
        return;
    }

    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
    std::string prefix;
    std::string tag;

    /*
     * Construct prefix in back-to-front order.
     */
    std::shared_ptr<log_context::label> label = _label;
    while (label) {
        if (label->log_relative_time) {
            if (tag.size() > 0) {
                prefix = "[" + tag + "] " + prefix;
                tag = "";
            }

            auto time_elapsed = timestamp - label->creation_time;
            long milliseconds_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(time_elapsed).count();
            long milliseconds = milliseconds_elapsed % 1000;
            long seconds_elapsed = milliseconds_elapsed / 1000;
            long seconds = seconds_elapsed % 60;
            long minutes_elapsed = seconds_elapsed / 60;
            long minutes = minutes_elapsed % 60;
            long hours = minutes_elapsed / 60;
            std::stringstream stream;
            stream << hours;
            stream << ":" << std::setfill('0') << std::setw(2) << minutes;
            stream << ":" << std::setfill('0') << std::setw(2) << seconds;
            stream << "." << std::setfill('0') << std::setw(3) << milliseconds;
            stream << " ";
            prefix = stream.str() + prefix;
        }

        if (label->description.size() > 0) {
            if (tag.size() > 0) {
                tag.insert(tag.begin(), '/');
            }
            tag.insert(tag.begin(), label->description.begin(), label->description.end());
        }

        label = label->parent;
    }
    if (tag.size() > 0) {
        prefix = "[" + tag + "] " + prefix;
    }

    boost::string_view::size_type start = 0;
    while (start < message.size()) {
        boost::string_view::size_type end = message.find('\n', start);
        boost::string_view::size_type next;
        if (end == boost::string_view::npos) {
            next = message.size();
        } else {
            next = end + 1;
        }
        _sink->log(level, prefix + std::string(message.substr(start, end - start)));
        start = next;
    }
}

bool log_context::tracker::would_log(log_level level) const
{
    if (!_sink) {
        return false;
    }
    return _sink->would_log(level);
}



log_context::log_context():
    _tracker(std::make_shared<log_context::tracker>(asio::executor(), nullptr, nullptr))
{}

log_context::log_context(asio::executor executor, Logger* sink):
    _tracker(std::make_shared<log_context::tracker>(executor, sink,
        std::make_shared<log_context::label>(nullptr, std::string())
    ))
{}

log_context log_context::tag(const std::string& description)
{
    return log_context(std::make_shared<log_context::tracker>(*_tracker,
        std::make_shared<log_context::label>(_tracker->label(), description)
    ));
}

log_context log_context::tag(std::string&& description)
{
    return log_context(std::make_shared<log_context::tracker>(*_tracker,
        std::make_shared<log_context::label>(_tracker->label(), description)
    ));
}

log_context log_context::tag(boost::string_view description)
{
    return log_context(std::make_shared<log_context::tracker>(*_tracker,
        std::make_shared<log_context::label>(_tracker->label(), description)
    ));
}

log_context log_context::tag(const char* description)
{
    return log_context(std::make_shared<log_context::tracker>(*_tracker,
        std::make_shared<log_context::label>(_tracker->label(), boost::string_view(description))
    ));
}

uint64_t log_context::make_id()
{
    static std::atomic_uint64_t next_id{0};
    return next_id++;
}

log_context log_context::track_lifetime(boost::optional<std::chrono::steady_clock::duration> duration)
{
    _tracker->log_relative_time(true);
    if (duration) {
        _tracker->start_watchdog(*duration);
    }
    _tracker->log_destruction(true);
    _tracker->log(VERBOSE, "Started");
    return *this;
}



} // ouinet namespace
