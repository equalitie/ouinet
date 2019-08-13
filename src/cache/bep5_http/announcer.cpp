#include "announcer.h"
#include "../../util/async_queue.h"
#include "../../logger.h"
#include "../../async_sleep.h"
#include "../../bittorrent/node_id.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::cache::bep5_http;

using namespace chrono_literals;

namespace bt = bittorrent;
using Clock = chrono::steady_clock;

//--------------------------------------------------------------------
// Entry

struct Entry {
    string debug_key;
    bt::NodeID infohash;

    Clock::time_point update_attempt;
    Clock::time_point update;

    Entry() = default;

    Entry(Announcer::Key key)
        : debug_key(move(key))
        , infohash(util::sha1_digest(debug_key))
    { }
};

//--------------------------------------------------------------------
// Loop
struct Announcer::Loop {
    asio::io_service& ios;
    shared_ptr<bt::MainlineDht> dht;
    util::AsyncQueue<Entry> entries;
    Cancel _cancel;
    Cancel _timer_cancel;

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

        entries.push_front(Entry(move(key)));
        _timer_cancel();
        _timer_cancel = Cancel();
    }

    Clock::duration next_update_after(const Entry& e) const
    {
        auto now = Clock::now();
        if (e.update + 10min < now) return 0s;
        if (e.update_attempt + 5min < now) return 0s;
        return 5min - (now - e.update_attempt);
    }

    Entry pick_entry(Cancel& cancel, asio::yield_context yield)
    {
        while (!cancel) {
            if (entries.empty()) {
                sys::error_code ec;
                entries.async_wait_for_push(cancel, yield[ec]);
                if (cancel) ec = asio::error::operation_aborted;
                if (ec) return or_throw<Entry>(yield, ec);
            }

            auto& f = entries.front();

            auto d = next_update_after(f);

            if (d == 0s) {
                Entry e = std::move(f);
                entries.pop();
                return e;
            }

            async_sleep(ios, d, _timer_cancel, yield);
        }

        return or_throw<Entry>(yield, asio::error::operation_aborted);
    }

    void start()
    {
        asio::spawn(dht->get_io_service(), [&] (asio::yield_context yield) {
            Cancel cancel(_cancel);
            loop(cancel, yield);
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
            auto e = pick_entry(cancel, yield[ec]);

            if (cancel) return;
            assert(!ec);
            ec = {};

            e.update_attempt = Clock::now();

            // Try inserting three times before moving to the next entry
            bool success = false;
            for (int i = 0; i != 3; ++i) {
                announce(e, cancel, yield[ec]);
                if (!ec) { success = true; break; }
                async_sleep(ios, chrono::seconds(1+i), cancel, yield[ec]);
                if (cancel) return;
                ec = {};
            }

            if (!success) {
                entries.push_back(move(e));
                continue;
            }

            e.update = Clock::now();

            entries.push_back(move(e));
        }

        return or_throw(yield, asio::error::operation_aborted);
    }

    void announce(Entry& e, Cancel& cancel, asio::yield_context yield)
    {
        LOG_DEBUG("Announcing ", e.debug_key);
        sys::error_code ec;
        dht->tracker_announce(e.infohash, boost::none, cancel, yield[ec]);
        LOG_DEBUG("Announcing ended ", e.debug_key, " ec:", ec.message());
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

Announcer::~Announcer() {}
