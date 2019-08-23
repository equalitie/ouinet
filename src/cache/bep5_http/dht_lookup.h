#pragma once

#include <functional>
#include <set>
#include "../../util/async_job.h"

namespace std {
    template<> struct hash<ouinet::bittorrent::NodeID> {
        using argument_type = ouinet::bittorrent::NodeID;
        using result_type = typename std::hash<std::string>::result_type;

        result_type operator()(argument_type const& a) const noexcept {
            return std::hash<std::string>{}(a.to_hex());
        }
    };
}

namespace ouinet { namespace cache { namespace bep5_http {

class DhtLookup {
private:
    using Clock = std::chrono::steady_clock;
    using udp = asio::ip::udp;
    using tcp = asio::ip::tcp;
    using Ret = std::set<udp::endpoint>;
    using Job = AsyncJob<boost::none_t>;
    using NodeID = bittorrent::NodeID;

    struct Result {
        sys::error_code   ec = asio::error::no_data;
        Ret               value;
        Clock::time_point time;

        bool is_fresh() const {
            using namespace std::chrono_literals;
            if (ec) return false;
            return time + 5min >= Clock::now();
        }
    };

    static Clock::duration timeout() {
#ifndef NDEBUG // debug
        return std::chrono::minutes(1);
#else // release
        return std::chrono::minutes(3);
#endif
    }

public:
    DhtLookup(DhtLookup&&) = delete;

    DhtLookup(std::weak_ptr<bittorrent::MainlineDht> dht_w, NodeID infohash)
        : infohash(infohash)
        , ioc(dht_w.lock()->get_io_service())
        , dht_w(dht_w)
        , cv(ioc)
    { }

    Ret get(Cancel c, asio::yield_context y) {
        // * Start a new job if one isn't already running
        // * Use previously returned result if it's not older than 5mins
        // * Otherwise wait for the running job to finish

        auto cancel_con = lifetime_cancel.connect([&] { c(); });

        if (!job) {
            job = make_job();
        }

        if (last_result.is_fresh()) {
            return last_result.value;
        }

#ifndef NDEBUG
        WatchDog wd(ioc, timeout() + std::chrono::seconds(5), [&] {
                LOG_ERROR("DHT BEP5 DhtLookup::get failed to time out");
            });
#endif

        sys::error_code ec;
        cv.wait(c, y[ec]);

        return_or_throw_on_error(y, c, ec, Ret{});

        return or_throw(y, last_result.ec, last_result.value);
    }

    ~DhtLookup() { lifetime_cancel(); }

private:

    std::unique_ptr<Job> make_job() {
        auto job = std::make_unique<Job>(ioc);

        job->start([ self = this
                   , dht_w = dht_w
                   , infohash = infohash
                   , lc = std::make_shared<Cancel>(lifetime_cancel)
                   ] (Cancel c, asio::yield_context y) mutable {
            auto cancel_con = lc->connect([&] { c(); });

            auto on_exit = defer([&] {
                    if (*lc) return;
                    self->cv.notify();
                    self->job = nullptr;
                });

            WatchDog wd(self->ioc, timeout(), [&] {
                    LOG_WARN("DHT BEP5 lookup ", infohash, " timed out");
                    c();
                });

            auto dht = dht_w.lock();
            assert(dht);

            if (!dht)
                return or_throw( y
                               , asio::error::operation_aborted
                               , boost::none);

            sys::error_code ec;

            auto eps = tcp_to_udp(dht->tracker_get_peers(infohash, c, y[ec]));

            if (!c && !ec) {
                self->last_result.ec    = ec;
                self->last_result.value = move(eps);
                self->last_result.time  = Clock::now();
            }

            return or_throw(y, ec, boost::none);
        });

        return job;
    }

    static Ret tcp_to_udp(const std::set<tcp::endpoint>& eps)
    {
        Ret ret;
        for (auto& ep : eps) { ret.insert({ep.address(), ep.port()}); }
        return ret;
    }

private:
    NodeID infohash;
    asio::io_context& ioc;
    std::weak_ptr<bittorrent::MainlineDht> dht_w;
    std::unique_ptr<Job> job;
    ConditionVariable cv;
    Result last_result;
    Cancel lifetime_cancel;
};


}}} // namespaces
