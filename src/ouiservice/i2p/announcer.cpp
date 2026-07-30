#include "announcer.h"
#include "tracker.h"
#include "task.h"
#include "util/async.h"
#include "bittorrent/node_id.h"
#include "logger.h"

#include <chrono>
#include <vector>

namespace ouinet {

using namespace std::chrono_literals;
using NodeID = bittorrent::NodeID;
using steady_clock = std::chrono::steady_clock;
using time_point = steady_clock::time_point;

static constexpr asio::steady_timer::duration period = 15min;

struct Entry {
    NodeID infohash;
    time_point announce_at;
};

struct I2pAnnouncer::State {
    asio::steady_timer timer;
    std::shared_ptr<I2pTrackerClient> tracker;
    Cancel cancel;
    std::vector<Entry> entries;

    State(std::shared_ptr<I2pTrackerClient> tracker):
        timer(tracker->get_executor()),
        tracker(std::move(tracker))
    {}

    asio::any_io_executor get_executor() {
        return tracker->get_executor();
    }

    // Find entry with lowest `announce_at`
    Entry* front() {
        Entry* ret = nullptr;
        for (auto& entry : entries) {
            if (ret == nullptr || entry.announce_at < ret->announce_at) {
                ret = &entry;
            }
        }
        return ret;
    }

    Entry* find_entry_to_announce() {
        auto entry = front();
        if (entry && entry->announce_at <= steady_clock::now()) {
            return entry;
        }
        return nullptr;
    }

    time_point sleep_until() {
        Entry* entry = front();
        if (entry) return entry->announce_at;
        else return time_point::max();
    }

    bool has_entry(const NodeID& infohash) {
        for (auto& entry : entries) {
            if (entry.infohash == infohash) return true;
        }
        return false;
    }

    bool add(const NodeID& infohash) {
        if (has_entry(infohash)) return false;
        entries.emplace_back(infohash, steady_clock::now());
        timer.cancel();
        return true;
    }

    bool remove(const NodeID& infohash) {
        for (auto i = entries.begin(); i != entries.end(); ++i) {
            if (i->infohash == infohash) {
                entries.erase(i);
                return true;
            }
        }
        return false;
    }

    void error_sleep(auto error_count, Async yield) {
        using N = std::decay_t<decltype(error_count)>;
        if (error_count == 0) return;
        asio::steady_timer t(get_executor());
        auto slot = yield.cancel_slot([&] { t.cancel(); });
        // TODO: Add some randomness
        t.expires_after(1s * (1 << std::min<N>(error_count - 1, 5)));
        t.async_wait(yield);
    }

    // TODO: Find the time `t` of how long it takes to do one announcement,
    // then collect all entries that are to be announced in the interval
    // between `now` and `now + t`. Then announce all those info-hashes in bulk
    // so we can reuse the connection to the tracker and announce faster.
    void loop(Async yield) {
        auto slot = yield.cancel_slot([&] { timer.cancel(); });

        while (true) {
            timer.expires_at(sleep_until());
            timer.async_wait(yield);

            uint16_t error_count = 0;

            while (true) {
                bool was_error = false;

                while (Entry* entry = find_entry_to_announce()) {
                    if (tracker->announce(entry->infohash, yield).has_value()) {
                        // Used by python test
                        LOG_DEBUG(yield, " BEP3 announced ", entry->infohash);
                        entry->announce_at = steady_clock::now() + period;
                    } else {
                        was_error = true;
                        break;
                    }
                }

                if (!was_error) break;
                else {
                    increment_without_overflow(error_count);
                    error_sleep(error_count, yield);
                }
            }
        }
    }

    static void increment_without_overflow(auto& v) {
        using T = std::decay_t<decltype(v)>;
        if (v == std::numeric_limits<T>::max()) return;
        ++v;
    }
};

I2pAnnouncer::I2pAnnouncer(std::shared_ptr<I2pTrackerClient> tracker):
    _state(std::make_shared<State>(std::move(tracker)))
{
    task::spawn_detached(
        _state->get_executor(),
        [state = _state] (asio::yield_context yield) {
            state->loop(
                Async(
                    yield,
                    state->cancel,
                    util::LogPath("I2pAnnouncer")));
        });
}

bool I2pAnnouncer::add(const NodeID& infohash) {
    return _state->add(infohash);
}

bool I2pAnnouncer::remove(const NodeID& infohash) {
    return _state->remove(infohash);
}

void I2pAnnouncer::close() {
    if (_state) _state->cancel();
}

I2pAnnouncer::~I2pAnnouncer() {
    close();
}

} // namespace
