#include "bep5_announcer.h"
#include "../async_sleep.h"
#include "../logger.h"
#include "../util/coro_tracker.h"
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

enum class Type { Periodic, Manual };

struct detail::Bep5AnnouncerImpl
    : public enable_shared_from_this<detail::Bep5AnnouncerImpl>
{
    Bep5AnnouncerImpl(NodeID infohash, std::weak_ptr<MainlineDht> dht_w, Type type)
        : type(type)
        , cv(dht_w.lock()->get_executor())
        , infohash(infohash)
        , dht_w(move(dht_w))
    {}

    void start()
    {
        auto self = shared_from_this();

        if (auto dht = dht_w.lock()) {
            auto exec = dht->get_executor();

            TRACK_SPAWN(exec, ([
                &, self, exec
            ] (asio::yield_context yield) mutable {
                loop(exec, yield);
            }));
        }
    }

    void loop(asio::executor& exec, asio::yield_context yield)
    {
        using namespace std::chrono_literals;

        UniformRandomDuration random_timeout;

        while (!cancel) {
            sys::error_code ec;

            if (type == Type::Manual) {
                while (!go_again) {
                    cv.wait(cancel, yield[ec]);
                    if (cancel) break;
                }
                go_again = false;
            }

            auto dht = dht_w.lock();
            if (!dht) return;

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

    void update() {
        if (type != Type::Manual) return;
        go_again = true;
        cv.notify();
    }

    Type type;
    ConditionVariable cv;
    bool go_again = false;
    NodeID infohash;
    weak_ptr<MainlineDht> dht_w;
    Cancel cancel;
    bool debug = false;
};

Bep5PeriodicAnnouncer::Bep5PeriodicAnnouncer( NodeID infohash
                                            , std::weak_ptr<MainlineDht> dht)
    : _impl(make_shared<detail::Bep5AnnouncerImpl>(infohash, move(dht), Type::Periodic))
{
    _impl->start();
}

Bep5PeriodicAnnouncer::~Bep5PeriodicAnnouncer()
{
    if (!_impl) return;
    _impl->cancel();
}

Bep5ManualAnnouncer::Bep5ManualAnnouncer( NodeID infohash
                                        , std::weak_ptr<MainlineDht> dht)
    : _impl(make_shared<detail::Bep5AnnouncerImpl>(infohash, move(dht), Type::Manual))
{
    _impl->start();
}

Bep5ManualAnnouncer::~Bep5ManualAnnouncer()
{
    if (!_impl) return;
    _impl->cancel();
}

void Bep5ManualAnnouncer::update()
{
    _impl->update();
}
