#include "bep5_announcer.h"
#include "../async_sleep.h"
#include "../logger.h"
#include "../task.h"
#include "../util/condition_variable.h"
#include "../util/debug.h"
#include "../util/wait_condition.h"
#include <chrono>
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

struct ouinet::bittorrent::detail::Bep5AnnouncerImpl
    : public enable_shared_from_this<detail::Bep5AnnouncerImpl>
{
    Bep5AnnouncerImpl( NodeID infohash
                     , std::weak_ptr<DhtBase> dht_w
                     , Type type
                     , util::LogPath log_path)
        : type(type)
        , cv(dht_w.lock()->get_executor())
        , infohash(infohash)
        , dht_w(move(dht_w))
        , log_path(log_path.tag("Bep5Announcer"))
    {}

    void start()
    {
        auto self = shared_from_this();

        if (auto dht = dht_w.lock()) {
            auto exec = dht->get_executor();

            task::spawn_detached(
                exec,
                [self = move(self)] (asio::yield_context y) mutable {
                    self->loop(Async(y, self->log_path));
                }
            );
        }
    }

    void loop(Async yield)
    {
        using namespace std::chrono_literals;

        auto cancel_con = cancel.connect([&] { yield.cancel(); });

        LOG_DEBUG(yield, " Start for infohash: ", infohash);

        UniformRandomDuration random_timeout;

        // Retry failed announces with exponential backoff strategy.
        const chrono::milliseconds min_fail_sleep(100);
        const chrono::milliseconds max_fail_sleep(60000);
        chrono::milliseconds fail_sleep = min_fail_sleep;


        while (true) {
            if (type == Type::Manual && !go_again) {
                LOG_DEBUG(yield, " Waiting for manual announce for infohash: ", infohash, "...");
                while (!go_again) {
                    cv.wait(yield);
                }
                LOG_DEBUG(yield, " Waiting for manual announce for infohash: ", infohash, ": done");
            }
            go_again = false;

            auto dht = dht_w.lock();
            if (!dht) return;

            if (!dht->all_ready()) {
                LOG_DEBUG(yield, " Waiting for DHT to get ready...");
                dht->wait_all_ready(yield);
                LOG_DEBUG(yield, " Waiting for DHT to get ready: done");
            }

            LOG_DEBUG(yield, " Announcing infohash ", infohash, "...");

            auto result = dht->tracker_announce(infohash, std::nullopt, yield);

            dht.reset();

            if (result) {
                LOG_DEBUG(yield, " Announcing infohash ", infohash, ": ok");
                fail_sleep = min_fail_sleep;
            } else {
                LOG_DEBUG(yield, " Announcing infohash ", infohash
                               , " failed: ", result.error().what()
                               , ". Retry in ", fail_sleep, "ms");

                async_sleep(fail_sleep, yield);
                fail_sleep = min(2 * fail_sleep, max_fail_sleep);

                go_again = true;  // do not wait for manual request
                continue;
            }

            if (type == Type::Manual) continue;  // wait for new manual request immediately

            // BEP5 indicatest that "After 15 minutes of inactivity, a node becomes questionable."
            // so try not to get too close to that value to avoid DHT churn
            // and the entry being frequently evicted from it.
            // Alternatively, set a closer period but use a normal (instead of uniform) distribution.
            auto sleep = debug ? random_timeout(2min, 4min) : random_timeout(5min, 12min);

            LOG_DEBUG(yield, " Waiting for ", chrono::duration_cast<chrono::seconds>(sleep).count()
                           , "s to announce infohash: ", infohash);

            async_sleep(sleep, yield);
        }
    }

    void update() {
        if (type != Type::Manual) return;
        LOG_DEBUG(log_path, " Manual update requested for infohash: ", infohash);
        go_again = true;
        cv.notify();
    }

    Type type;
    ConditionVariable cv;
    bool go_again = false;
    NodeID infohash;
    weak_ptr<DhtBase> dht_w;
    util::LogPath log_path;
    Cancel cancel;
    static const bool debug = false;  // for development testing only
};

Bep5PeriodicAnnouncer::Bep5PeriodicAnnouncer( NodeID infohash
                                            , std::weak_ptr<DhtBase> dht
                                            , util::LogPath log_path)
    : _impl(make_shared<detail::Bep5AnnouncerImpl>( infohash
                                                  , move(dht)
                                                  , Type::Periodic
                                                  , std::move(log_path)))
{
    _impl->start();
}

Bep5PeriodicAnnouncer::~Bep5PeriodicAnnouncer()
{
    if (!_impl) return;
    _impl->cancel();
}

Bep5ManualAnnouncer::Bep5ManualAnnouncer( NodeID infohash
                                        , std::weak_ptr<DhtBase> dht
                                        , util::LogPath log_path)
    : _impl(make_shared<detail::Bep5AnnouncerImpl>( infohash
                                                  , move(dht)
                                                  , Type::Manual
                                                  , std::move(log_path)))
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
