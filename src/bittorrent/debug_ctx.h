#pragma once

#include <chrono>
#include <ostream>
#include "../util/str.h"

namespace ouinet { namespace bittorrent {

struct DebugCtx {
    using Clock = std::chrono::steady_clock;

    size_t id;
    bool enable_log = false;
    Clock::time_point start;

    float uptime() const { return secs(start); }

    DebugCtx()
        : id(gen_id())
        , start(Clock::now())
    {}

    static
    float secs(Clock::time_point start) {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now() - start).count()
             / 1000.f;
    };

    operator bool() const { return enable_log; }

    template<class... Args>
    void log(Args&&... args) {
        std::cerr << "DebugCtx:" << id << " ";
        std::cerr << std::fixed
                  << std::setw(10)
                  << std::setprecision(5)
                  << uptime() << "s ";
        util::args_to_stream(std::cerr, std::forward<Args>(args)...);
        std::cerr << "\n";
    }

    void enable() { enable_log = true; }

private:
    static size_t gen_id() {
        static decltype(id) next_id = 0;
        return next_id++;
    }
};

}} // namespaces
