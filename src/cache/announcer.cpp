#include <list>
#include <sstream>
#include <iomanip>
#include "announcer.h"
#include "util/async_queue.h"
#include "logger.h"
#include "defer.h"
#include "../util/wait_condition.h"
#include "async_sleep.h"
#include "bittorrent/node_id.h"
#include "util/handler_tracker.h"
#include <boost/utility/string_view.hpp>

#define _LOGPFX "Announcer: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)

using namespace std;
using namespace ouinet;
using namespace ouinet::cache;

using namespace chrono_literals;

namespace bt = bittorrent;
using Clock = chrono::steady_clock;

//--------------------------------------------------------------------
// Entry

struct Entry {
    string key;
    bt::NodeID infohash;

    Clock::time_point successful_update;
    Clock::time_point failed_update;

    bool to_remove = false;

    Entry() = default;

    Entry(Announcer::Key key)
        : key(move(key))
        , infohash(util::sha1_digest(this->key))
    { }

    bool attempted_update() const {
        return successful_update != Clock::time_point()
            || failed_update     != Clock::time_point();
    }
};

//--------------------------------------------------------------------
// Base Loop
struct Announcer::Loop {
    using Entries = util::AsyncQueue<Entry, std::list>;

    AsioExecutor ex;
    Entries entries;
    size_t _simultaneous_announcements;
    Cancel _cancel;
    Cancel _timer_cancel;

    static Clock::duration success_reannounce_period() { return 20min; }
    static Clock::duration failure_reannounce_period() { return 5min;  }

    Loop(AsioExecutor ex, size_t simultaneous_announcements)
        : ex(ex)
        , entries(ex)
        , _simultaneous_announcements(simultaneous_announcements)
    { }

    inline static bool debug() { return logger.get_threshold() <= DEBUG; }

    Entries::iterator find_entry_by_key(const Key& key) {
        for (auto i = entries.begin(); i != entries.end(); ++i) {
            if (i->first.key == key) return i;
        }
        return entries.end();
    }

    bool add(Key key) {
        auto entry_i = find_entry_by_key(key);
        bool already_has_key = (entry_i != entries.end());

        if (already_has_key) {
            _DEBUG("Adding ", key, " (already exists)");
            entry_i->first.to_remove = false;
        } else {
            _DEBUG("Adding ", key);
        }

        if (already_has_key) return false;

        // To preserve the order in which entries are added and updated we put
        // this new entry _after_ all entries that have not yet been updated.
        Entries::iterator i = entries.begin();

        for (; i != entries.end(); ++i) {
            const auto& e = i->first;
            if (e.attempted_update()) break;
        }

        entries.insert(i, Entry(move(key)));
        _timer_cancel();
        _timer_cancel = Cancel();
        return true;
    }

    bool remove(const Key& key) {
        Entries::iterator i = entries.begin();

        for (; i != entries.end(); ++i)
            if (i->first.key == key) break;  // found
        if (i == entries.end()) return false;  // not found

        _DEBUG("Marking ", key, " for removal");
        // The actual removal is not done here but in the main loop.
        i->first.to_remove = true;
        // No new entries, so no `_timer_cancel` reset.
        return true;
    }

    Clock::duration next_update_after(const Entry& e) const
    {
        if (e.successful_update == Clock::time_point()
             && e.failed_update == Clock::time_point()) {
            return 0s;
        }

        auto now = Clock::now();

        if (e.successful_update >= e.failed_update) {
            auto p = success_reannounce_period();
            if (e.successful_update + p <= now) return 0s;
            return e.successful_update + p - now;
        }
        else {
            auto p = failure_reannounce_period();
            if (e.failed_update + p < now) return 0s;
            return e.failed_update + p - now;
        }
    }

    void print_entries() const {
        auto now = Clock::now();
        ostringstream ss;
        auto print = [&] (Clock::time_point t) {
            if (t == Clock::time_point()) {
                ss << "--:--:--";
            }
            else {
                // TODO: For the purpose of analyzing logs, it would be better
                // to print absolute times.
                using namespace std::chrono;
                unsigned secs = duration_cast<milliseconds>(now - t).count() / 1000.f;
                unsigned hrs  = secs / (60*60);
                secs -= hrs * 60*60;
                unsigned mins = secs / 60;
                secs -= mins * 60;

                ss << std::setfill('0') << std::setw(2) << hrs;
                ss << ':';
                ss << std::setfill('0') << std::setw(2) << mins;
                ss << ':';
                ss << std::setfill('0') << std::setw(2) << secs;
            }
            ss << " ago";
        };

        _DEBUG("Entries:");
        for (auto& ep : entries) {
            auto& e = ep.first;
            ss << " " << e.infohash << " | successful_update=";
            print(e.successful_update);
            ss << " | failed_update=";
            print(e.failed_update);
            ss << " | key=" << e.key;

            _DEBUG(ss.str());
            ss.str({});
        }
    }

    Entries::iterator pick_entry(Cancel& cancel, asio::yield_context yield)
    {
        auto end = entries.end();

        while (!cancel) {
            if (entries.empty()) {
                // XXX: Temporary handler tracking as this coroutine sometimes
                // fails to exit.
                TRACK_HANDLER();
                sys::error_code ec;
                _DEBUG("No entries to update, waiting...");
                entries.async_wait_for_push(cancel, yield[ec]);
                return_or_throw_on_error(yield, cancel, ec, end);
            }

            assert(!entries.empty());

            auto i = entries.begin();

            auto d = next_update_after(i->first);

            _DEBUG( "Found entry to update. It'll be updated in "
                  , chrono::duration_cast<chrono::seconds>(d).count()
                  , " seconds: ", i->first.key);

            if (d == 0s) return i;

            auto cc = cancel.connect([&] { _timer_cancel(); });
            async_sleep(ex, d, _timer_cancel, yield);
        }

        return or_throw(yield, asio::error::operation_aborted, end);
    }

    void start()
    {
        TRACK_SPAWN(ex, [this] (asio::yield_context yield) {
            Cancel cancel(_cancel);
            sys::error_code ec;
            loop(cancel, yield[ec]);
        });
    }

    void loop(Cancel& cancel, asio::yield_context yield)
    {
        auto on_exit = defer([&] {
            _DEBUG("Exiting the loop; cancel=", (cancel ? "true":"false"));
        });

        WaitCondition wcon(ex);

        while (!cancel) {
            sys::error_code ec_wcon;

            for (size_t n = 0; n < _simultaneous_announcements; ++n) {
                sys::error_code ec;
                _DEBUG("Picking entry to update");
                auto ei = pick_entry(cancel, yield[ec]);

                if (cancel) return;
                assert(!ec);
                ec = {};

                if (ei->first.to_remove) {
                    // Marked for removal, drop the entry and get another one.
                    entries.erase(ei);
                    continue;
                }

                TRACK_SPAWN(ex, ([this, &cancel, &wcon, lock = wcon.lock()] (asio::yield_context yield) {
                    sys::error_code ec_coro;

                    // Try inserting three times before moving to the next entry
                    bool success = false;

                    Entry e = move(entries.begin()->first);
                    for (int i = 0; i != 3; ++i) {
                        // XXX: Temporary handler tracking as this coroutine sometimes
                        // fails to exit.
                        TRACK_HANDLER();
                        announce(e, cancel, yield[ec_coro]);
                        if (cancel) return;
                        if (!ec_coro) { success = true; break; }
                        async_sleep(ex, chrono::seconds(1+i), cancel, yield[ec_coro]);
                        if (cancel) return;
                        ec_coro = {};
                    }

                    if (success) {
                        e.failed_update     = {};
                        e.successful_update = Clock::now();
                    } else  {
                        e.failed_update     = Clock::now();
                    }

                    if (!e.to_remove) entries.push_back(move(e));
                    if (debug()) { print_entries(); }
                }));

                entries.erase(ei);
            }

            wcon.wait(yield[ec_wcon]);
        }

        return or_throw(yield, asio::error::operation_aborted);
    }

    // Virtual announce method - to be overridden by children
    virtual void announce(Entry& e, Cancel& cancel, asio::yield_context yield) = 0;

    virtual ~Loop() { _cancel(); }
};

//--------------------------------------------------------------------
// Bep5Loop - announces to DHT
struct Bep5Loop : public Announcer::Loop {
    shared_ptr<bt::DhtBase> dht;

    Bep5Loop(shared_ptr<bt::DhtBase> dht, size_t simultaneous_announcements)
        : Loop(dht->get_executor(), simultaneous_announcements)
        , dht(move(dht))
    { }

    void start()
    {
        TRACK_SPAWN(ex, [this] (asio::yield_context yield) {
            Cancel cancel(_cancel);
            sys::error_code ec;

            // Wait for DHT to be ready before starting the loop
            {
                TRACK_HANDLER();
                _DEBUG("Waiting for DHT");
                dht->wait_all_ready(cancel, yield[ec]);
            }

            loop(cancel, yield[ec]);
        });
    }

    void announce(Entry& e, Cancel& cancel, asio::yield_context yield) override
    {
        _DEBUG("Announcing (BEP5/DHT): ", e.key, "...");

        sys::error_code ec;
        auto e_key{debug() ? e.key : ""};  // cancellation trashes the key
        dht->tracker_announce(e.infohash, boost::none, cancel, yield[ec]);

        _DEBUG("Announcing (BEP5/DHT): ", e_key, ": done; ec=", ec);

        return or_throw(yield, ec);
    }
};

#ifdef __EXPERIMENTAL__
//--------------------------------------------------------------------
// Bep3Loop - announces via HTTP to tracker
struct Bep3Loop : public Announcer::Loop {
    string tracker_url;

    Bep3Loop(AsioExecutor ex, string tracker_url, size_t simultaneous_announcements)
        : Loop(ex, simultaneous_announcements)
        , tracker_url(move(tracker_url))
    { }

    void announce(Entry& e, Cancel& cancel, asio::yield_context yield) override
    {
        _DEBUG("Announcing (BEP3/HTTP): ", e.key, " to tracker ", tracker_url, "...");

        sys::error_code ec;
        auto e_key{debug() ? e.key : ""};  // cancellation trashes the key

        // TODO: Implement HTTP announce to tracker
        // This should make an HTTP GET request to the tracker with:
        // - info_hash: e.infohash
        // - peer_id: generated peer id
        // - port: listening port
        // - event: started/completed/stopped

        _DEBUG("Announcing (BEP3/HTTP): ", e_key, ": done; ec=", ec);

        return or_throw(yield, ec);
    }
};
#endif // __EXPERIMENTAL__

//--------------------------------------------------------------------
// Base Announcer
Announcer::Announcer(AsioExecutor ex, size_t simultaneous_announcements)
    : _loop(nullptr)
{
}

bool Announcer::add(Key key)
{
    return _loop->add(move(key));
}

bool Announcer::remove(const Key& key) {
    return _loop->remove(key);
}

Announcer::~Announcer() {}

//--------------------------------------------------------------------
// Bep5Announcer
Bep5Announcer::Bep5Announcer(std::shared_ptr<bittorrent::DhtBase> dht, size_t simultaneous_announcements)
    : Announcer(dht->get_executor(), simultaneous_announcements)
{
    _loop = make_unique<Bep5Loop>(move(dht), simultaneous_announcements);
    static_cast<Bep5Loop*>(_loop.get())->start();
}

Bep5Announcer::~Bep5Announcer() {}

#ifdef __EXPERIMENTAL__
//--------------------------------------------------------------------
// Bep3Announcer
Bep3Announcer::Bep3Announcer(AsioExecutor ex, std::string tracker_url, size_t simultaneous_announcements)
    : Announcer(ex, simultaneous_announcements)
{
    _loop = make_unique<Bep3Loop>(ex, move(tracker_url), simultaneous_announcements);
    _loop->start();
}

Bep3Announcer::~Bep3Announcer() {}
#endif // __EXPERIMENTAL__
