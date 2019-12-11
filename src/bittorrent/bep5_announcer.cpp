#include "bep5_announcer.h"
#include "../async_sleep.h"
#include "../logger.h"
#include <random>
#include <iostream>

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;

class UniformRandomDuration {
public:
    using Duration = std::chrono::milliseconds;

    UniformRandomDuration()
        : gen(rd())
    { }

    Duration operator()(Duration min, Duration max)
    {
        assert(max >= min);
        uniform_int_distribution<Duration::rep> dis(0, (max-min).count());
        return min + Duration(dis(gen));
    }

private:
    std::random_device rd;
    mt19937 gen;
};

struct Bep5PeriodicAnnouncer::Impl
    : public enable_shared_from_this<Bep5PeriodicAnnouncer::Impl>
{
    Impl(NodeID infohash, std::weak_ptr<MainlineDht> dht_w)
        : infohash(infohash)
        , dht_w(move(dht_w))
    {}

    void start()
    {
        auto self = shared_from_this();

        if (auto dht = dht_w.lock()) {
            auto exec = dht->get_executor();

            asio::spawn( exec
                       , [&, self, exec] (asio::yield_context yield) mutable {
                             loop(exec, yield);
                         });
        }
    }

    void loop(asio::executor& exec, asio::yield_context yield)
    {
        using namespace std::chrono_literals;

        UniformRandomDuration random_timeout;

        while (!cancel) {
            auto dht = dht_w.lock();
            if (!dht) return;

            sys::error_code ec;

            if (debug) {
                LOG_DEBUG("ANNOUNCING ", infohash, " ...");
            }

            dht->tracker_announce(infohash, boost::none, cancel, yield[ec]);

            if (debug) {
                LOG_DEBUG("ANNOUNCING ", infohash, " done: ", ec.message(), " cancel:", bool(cancel));
            }

            if (cancel) return;

            dht.reset();

            if (ec) {
                // TODO: Arbitrary timeout
                async_sleep(exec, random_timeout(1s, 1min), cancel, yield);
                if (cancel) return;
                continue;
            }

            auto sleep = random_timeout(5min, 30min);

            if (debug) {
                LOG_DEBUG("ANNOUNCING ", infohash, " next in: ", (sleep.count()/1000.f), "s");
            }

            async_sleep(exec, sleep, cancel, yield);
        }
    }

    NodeID infohash;
    weak_ptr<MainlineDht> dht_w;
    Cancel cancel;
    bool debug = false;
};

Bep5PeriodicAnnouncer::Bep5PeriodicAnnouncer( NodeID infohash
                                            , std::weak_ptr<MainlineDht> dht)
    : _impl(make_shared<Impl>(infohash, move(dht)))
{
    _impl->start();
}

Bep5PeriodicAnnouncer::~Bep5PeriodicAnnouncer()
{
    if (!_impl) return;
    _impl->cancel();
}
