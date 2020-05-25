#include <list>
#include "announcer.h"
#include "../../util/async_queue.h"
#include "../../logger.h"
#include "../../async_sleep.h"
#include "../../bittorrent/node_id.h"
#include "../../util/handler_tracker.h"
#include <boost/utility/string_view.hpp>

using namespace std;
using namespace ouinet;
using namespace ouinet::cache;

using namespace chrono_literals;

namespace bt = bittorrent;
using Clock = chrono::steady_clock;

struct LogLevel {
    LogLevel(log_level_t ll)
        : _ll(make_shared<log_level_t>(ll))
    {}

    bool debug() const {
        return (*_ll <= DEBUG)
            || (logger.get_log_file() != nullptr);
    }

    std::shared_ptr<log_level_t> _ll;
};

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
// Loop
struct Announcer::Loop {
    using Entries = util::AsyncQueue<Entry, std::list>;

    asio::executor ex;
    shared_ptr<bt::MainlineDht> dht;
    Entries entries;
    Cancel _cancel;
    Cancel _timer_cancel;
    LogLevel _log_level;

    void set_log_level(log_level_t l) { *_log_level._ll = l; }

    static Clock::duration success_reannounce_period() { return 20min; }
    static Clock::duration failure_reannounce_period() { return 5min;  }

    Loop(shared_ptr<bt::MainlineDht> dht, log_level_t log_level)
        : ex(dht->get_executor())
        , dht(move(dht))
        , entries(ex)
        , _log_level(log_level)
    { }

    bool already_has(const Key& key) const {
        for (auto& e : entries) {
            if (e.first.key == key) return true;
        }
        return false;
    }

    void add(Key key) {
        bool already_has_key = already_has(key);

        if (_log_level.debug()) {
            if (already_has_key) {
                std::cerr << "Announcer: adding " << key << " (already exists)\n";
            } else {
                std::cerr << "Announcer: adding " << key << "\n";
            }
        }

        if (already_has_key) return;

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
    }

    void remove(const Key& key) {
        Entries::iterator i = entries.begin();

        for (; i != entries.end(); ++i)
            if (i->first.key == key) break;  // found
        if (i == entries.end()) return;  // not found

        // The actual removal is not done here but in the main loop.
        i->first.to_remove = true;
        // No new entries, so no `_timer_cancel` reset.
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
        auto print = [&] (Clock::time_point t) {
            if (t == Clock::time_point()) {
                cerr << "--:--:--";
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

                cerr << std::setfill('0') << std::setw(2) << hrs;
                cerr << ':';
                cerr << std::setfill('0') << std::setw(2) << mins;
                cerr << ':';
                cerr << std::setfill('0') << std::setw(2) << secs;
            }
            cerr << " ago";
        };

        cerr << "Announcer: entries:" << "\n";
        for (auto& ep : entries) {
            auto& e = ep.first;
            cerr << "Announcer:  " << e.infohash << " | successful_update:";
            print(e.successful_update);
            cerr << " | failed_update:";
            print(e.failed_update);
            cerr << " | key:" << e.key << "\n";
        }
    }

    Entries::iterator pick_entry(Cancel& cancel, asio::yield_context yield)
    {
        LogLevel ll = _log_level;

        auto end = entries.end();

        while (!cancel) {
            if (entries.empty()) {
                // XXX: Temporary handler tracking as this coroutine sometimes
                // fails to exit.
                TRACK_HANDLER();
                sys::error_code ec;
                if (ll.debug()) {
                    std::cerr << "Announcer: no entries to update, waiting...\n";
                }
                entries.async_wait_for_push(cancel, yield[ec]);
                if (cancel) ec = asio::error::operation_aborted;
                if (ec) return or_throw(yield, ec, end);
            }

            assert(!entries.empty());

            auto i = entries.begin();

            auto d = next_update_after(i->first);

            if (ll.debug()) {
                std::cerr << "Announcer: found entry to update. It'll be updated in "
                          << chrono::duration_cast<chrono::seconds>(d).count()
                          << " seconds; " << i->first.key << "\n";
            }

            if (d == 0s) return i;

            auto cc = cancel.connect([&] { _timer_cancel(); });
            async_sleep(ex, d, _timer_cancel, yield);
        }

        return or_throw(yield, asio::error::operation_aborted, end);
    }

    void start()
    {
        TRACK_SPAWN(dht->get_executor(), [&] (asio::yield_context yield) {
            Cancel cancel(_cancel);
            sys::error_code ec;
            loop(cancel, yield[ec]);
        });
    }

    void loop(Cancel& cancel, asio::yield_context yield)
    {
        LogLevel ll = _log_level;

        {
            // XXX: Temporary handler tracking as this coroutine sometimes
            // fails to exit.
            TRACK_HANDLER();
            sys::error_code ec;
            if (ll.debug()) cerr << "Announcer: waiting for DHT\n";
            dht->wait_all_ready(cancel, yield[ec]);
        }

        auto on_exit = defer([&] {
            if (ll.debug()) {
                if (ll.debug()) cerr << "Announcer: exiting the loop "
                                        "(cancel:" << (cancel ? "true":"false") << "\n";
            }
        });

        while (!cancel) {
            sys::error_code ec;
            if (ll.debug()) cerr << "Announcer: picking entry to update\n";
            auto ei = pick_entry(cancel, yield[ec]);

            if (cancel) return;
            assert(!ec);
            ec = {};

            if (ei->first.to_remove) {
                // Marked for removal, drop the entry and get another one.
                entries.erase(ei);
                continue;
            }

            // Try inserting three times before moving to the next entry
            bool success = false;
            for (int i = 0; i != 3; ++i) {
                // XXX: Temporary handler tracking as this coroutine sometimes
                // fails to exit.
                TRACK_HANDLER();
                announce(ei->first, cancel, yield[ec]);
                if (cancel) return;
                if (!ec) { success = true; break; }
                async_sleep(ex, chrono::seconds(1+i), cancel, yield[ec]);
                if (cancel) return;
                ec = {};
            }

            if (success) {
                ei->first.failed_update     = {};
                ei->first.successful_update = Clock::now();
            } else  {
                ei->first.failed_update     = Clock::now();
            }

            Entry e = move(ei->first);
            entries.erase(ei);
            if (!e.to_remove) entries.push_back(move(e));

            if (ll.debug()) { print_entries(); }
        }

        return or_throw(yield, asio::error::operation_aborted);
    }

    void announce(Entry& e, Cancel& cancel, asio::yield_context yield)
    {
        auto ll = _log_level;

        if (ll.debug()) {
            cerr << "Announcer: Announcing " << e.key << "\n";
        }

        sys::error_code ec;
        dht->tracker_announce(e.infohash, boost::none, cancel, yield[ec]);

        if (ll.debug()) {
            cerr << "Announcer: Announcing ended " << e.key << " ec:" << ec.message() << "\n";
        }

        return or_throw(yield, ec);
    }

    ~Loop() { _cancel(); }
};

//--------------------------------------------------------------------
// Announcer
Announcer::Announcer( std::shared_ptr<bittorrent::MainlineDht> dht
                    , log_level_t log_level)
    : _loop(new Loop(std::move(dht), log_level))
{
    _loop->start();
}

void Announcer::add(Key key)
{
    _loop->add(move(key));
}

void Announcer::remove(const Key& key) {
    _loop->remove(key);
}

void Announcer::set_log_level(log_level_t l)
{
    _loop->set_log_level(l);
}

Announcer::~Announcer() {}
