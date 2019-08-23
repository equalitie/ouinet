#include "announcer.h"
#include "../../util/async_queue.h"
#include "../../logger.h"
#include "../../async_sleep.h"
#include "../../bittorrent/node_id.h"
#include <boost/utility/string_view.hpp>

using namespace std;
using namespace ouinet;
using namespace ouinet::cache::bep5_http;

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

    asio::io_service& ios;
    shared_ptr<bt::MainlineDht> dht;
    Entries entries;
    Cancel _cancel;
    Cancel _timer_cancel;
    log_level_t log_level = INFO;

    void set_log_level(log_level_t l) { log_level = l; }

    bool log_debug() const { return log_level <= DEBUG; }

    static Clock::duration success_reannounce_period() { return 20min; }
    static Clock::duration failure_reannounce_period() { return 5min;  }

    Loop(shared_ptr<bt::MainlineDht> dht)
        : ios(dht->get_io_service())
        , dht(move(dht))
        , entries(ios)
    { }

    bool already_has(const Key& key) const {
        for (auto& e : entries) {
            if (e.first.key == key) return true;
        }
        return false;
    }

    void add(Key key) {
        if (already_has(key)) return;

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

        cerr << "BEP5 HTTP announcer entries:" << "\n";
        for (auto& ep : entries) {
            auto& e = ep.first;
            cerr << "  " << e.infohash << " | successful_update:";
            print(e.successful_update);
            cerr << " | failed_update:";
            print(e.failed_update);
            cerr << " | key:" << e.key << "\n";
        }
    }

    Entries::iterator pick_entry(Cancel& cancel, asio::yield_context yield)
    {
        auto end = entries.end();

        while (!cancel) {
            if (entries.empty()) {
                sys::error_code ec;
                entries.async_wait_for_push(cancel, yield[ec]);
                if (cancel) ec = asio::error::operation_aborted;
                if (ec) return or_throw(yield, ec, end);
            }

            auto i = entries.begin();

            auto d = next_update_after(i->first);

            if (d == 0s) { return i; }

            async_sleep(ios, d, _timer_cancel, yield);
        }

        return or_throw(yield, asio::error::operation_aborted, end);
    }

    void start()
    {
        asio::spawn(dht->get_io_service(), [&] (asio::yield_context yield) {
            Cancel cancel(_cancel);
            sys::error_code ec;
            loop(cancel, yield[ec]);
        });
    }

    void loop(Cancel& cancel, asio::yield_context yield)
    {
        {
            sys::error_code ec;
            dht->wait_all_ready(cancel, yield[ec]);
        }

        while (!cancel) {
            sys::error_code ec;
            auto ei = pick_entry(cancel, yield[ec]);

            if (cancel) return;
            assert(!ec);
            ec = {};

            // Try inserting three times before moving to the next entry
            bool success = false;
            for (int i = 0; i != 3; ++i) {
                announce(ei->first, cancel, yield[ec]);
                if (!ec) { success = true; break; }
                async_sleep(ios, chrono::seconds(1+i), cancel, yield[ec]);
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
            entries.push_back(move(e));

            if (log_debug()) { print_entries(); }
        }

        return or_throw(yield, asio::error::operation_aborted);
    }

    void announce(Entry& e, Cancel& cancel, asio::yield_context yield)
    {
        if (log_debug()) {
            cerr << "Announcing " << e.key << "\n";
        }

        sys::error_code ec;
        dht->tracker_announce(e.infohash, boost::none, cancel, yield[ec]);

        if (log_debug()) {
            cerr << "Announcing ended " << e.key << " ec:" << ec.message() << "\n";
        }

        return or_throw(yield, ec);
    }

    ~Loop() { _cancel(); }
};

//--------------------------------------------------------------------
// Announcer
Announcer::Announcer(std::shared_ptr<bittorrent::MainlineDht> dht)
    : _loop(new Loop(std::move(dht)))
{
    _loop->start();
}

void Announcer::add(Key key)
{
    _loop->add(move(key));
}

void Announcer::set_log_level(log_level_t l)
{
    _loop->set_log_level(l);
}

Announcer::~Announcer() {}
