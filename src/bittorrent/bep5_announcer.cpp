#include "bep5_announcer.h"
#include "../async_sleep.h"
#include <random>
#include <iostream>

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;

class UniformRandomDuration {
public:
    using Duration = std::chrono::milliseconds;

    UniformRandomDuration(Duration min, Duration max)
        : min(min)
        , gen(rd())
        , dis(0, (max - min).count())
    {
        assert(max >= min);
    }

    Duration operator()()
    {
        return min + Duration(dis(gen));
    }

private:
    Duration min;
    std::random_device rd;
    mt19937 gen;
    uniform_int_distribution<Duration::rep> dis;
};

struct Bep5Announcer::Impl
    : public enable_shared_from_this<Bep5Announcer::Impl>
{
    Impl(NodeID infohash, std::weak_ptr<MainlineDht> dht_w)
        : infohash(infohash)
        , dht_w(move(dht_w))
    {}

    void start()
    {
        auto self = shared_from_this();

        if (auto dht = dht_w.lock()) {
            ios = &dht->get_io_service();

            asio::spawn( *ios
                       , [&, self] (asio::yield_context yield) {
                             loop(yield);
                         });
        }
    }

    void loop(asio::yield_context yield)
    {
        using namespace std::chrono_literals;

        UniformRandomDuration random_timeout(5min, 30min);

        while (!cancel) {
            auto dht = dht_w.lock();
            if (!dht) return;

            sys::error_code ec;
            cerr << "ANNOUNCING...\n";
            dht->tracker_announce(infohash, boost::none, cancel, yield[ec]);
            cerr << "ANNOUNCING done: " << ec.message() << " cancel:" << bool(cancel) << "\n";

            if (cancel) return;

            dht.reset();

            if (ec) {
                // TODO: Arbitrary timeout
                async_sleep(*ios, 10s, cancel, yield);
                if (cancel) return;
                continue;
            }

            auto sleep = random_timeout();
            cerr << "ANNOUNCING next in: " << (sleep.count()/1000.f) << "s\n";
            async_sleep(*ios, sleep, cancel, yield);
        }
    }

    NodeID infohash;
    weak_ptr<MainlineDht> dht_w;
    asio::io_service* ios = nullptr;
    Cancel cancel;
};

Bep5Announcer::Bep5Announcer(NodeID infohash, std::weak_ptr<MainlineDht> dht)
    : _impl(make_shared<Impl>(infohash, move(dht)))
{
    _impl->start();
}

Bep5Announcer::~Bep5Announcer()
{
    if (!_impl) return;
    _impl->cancel();
}
