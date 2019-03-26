#pragma once

#include <chrono>
#include <ostream>

namespace ouinet { namespace bittorrent {

struct DebugCtx {
    using Clock = std::chrono::steady_clock;

    bool enable_log = false;
    Clock::time_point start;
    std::string tag;

    float uptime() const { return secs(start); }

    DebugCtx()
        : start(Clock::now())
    {}

    static
    float secs(auto start) {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now() - start).count()
             / 1000.f;
    };

    operator bool() const { return enable_log; }
};

inline
std::ostream& operator<<(std::ostream& os, const DebugCtx& dbg) {
    return os << dbg.tag << " " << dbg.uptime() << " ";
}

}} // namespaces
