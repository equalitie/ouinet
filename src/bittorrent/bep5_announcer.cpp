#include "bep5_announcer.h"
#include "../async_sleep.h"
#include "../logger.h"
#include "../util/handler_tracker.h"
#include "../util/quote_error_message.h"
#include <random>
#include <iostream>

#define _LOGPFX "Bep5Announcer: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _WARN(...) LOG_WARN(_LOGPFX, __VA_ARGS__)

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

        _DEBUG("Start for infohash: ", infohash);

        UniformRandomDuration random_timeout;

        while (!cancel) {
            if (type == Type::Manual && !go_again) {
                _DEBUG("Waiting for manual announce for infohash: ", infohash, "...");
                while (!go_again) {
                    sys::error_code ec;
                    cv.wait(cancel, yield[ec]);
                    if (cancel) return;
                }
                _DEBUG("Waiting for manual announce for infohash: ", infohash, ": done");
            }
            go_again = false;

            auto dht = dht_w.lock();
            if (!dht) return;

            _DEBUG("Announcing infohash: ", infohash, "...");

            sys::error_code ec;
            dht->tracker_announce(infohash, boost::none, cancel, yield[ec]);

            if (cancel) return;

            dht.reset();

            if (ec) {
                _WARN("Announcing infohash: ", infohash, ": failed; ec=", ec);
                // TODO: Arbitrary timeout
                _DEBUG("Will retry infohash because of announcement error: ", infohash);
                async_sleep(exec, random_timeout(1s, 1min), cancel, yield);
                if (cancel) return;
                go_again = true;  // do not wait for manual request
                continue;
            }

            _DEBUG("Announcing infohash: ", infohash, ": done");

            if (type == Type::Manual) continue;  // wait for new manual request immediately

            // BEP5 indicatest that "After 15 minutes of inactivity, a node becomes questionable."
            // so try not to get too close to that value to avoid DHT churn
            // and the entry being frequently evicted from it.
            // Alternatively, set a closer period but use a normal (instead of uniform) distribution.
            auto sleep = debug ? random_timeout(2min, 4min) : random_timeout(5min, 12min);

            _DEBUG( "Waiting for ", chrono::duration_cast<chrono::seconds>(sleep).count()
                  , "s to announce infohash: ", infohash);

            async_sleep(exec, sleep, cancel, yield);
        }
    }

    void update() {
        if (type != Type::Manual) return;
        _DEBUG("Manual update requested for infohash: ", infohash);
        go_again = true;
        cv.notify();
    }

    Type type;
    ConditionVariable cv;
    bool go_again = false;
    NodeID infohash;
    weak_ptr<MainlineDht> dht_w;
    Cancel cancel;
    static const bool debug = false;  // for development testing only
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
