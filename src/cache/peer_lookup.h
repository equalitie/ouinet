#pragma once

#include <set>
#include <string>
#include <util/async_job.h>
#include <util/compat.h>
#include <util/hash.h>
#include <bittorrent/node_id.h>

namespace ouinet { namespace cache {

template<typename PeerSet>
class PeerLookup {
protected:
    using Clock = std::chrono::steady_clock;
    using Ret = PeerSet;
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

    static Clock::duration timeout_duration() {
#ifndef NDEBUG // debug
        return std::chrono::minutes(1);
#else // release
        return std::chrono::minutes(3);
#endif
    }

public:
    PeerLookup(PeerLookup&&) = delete;

    PeerLookup(std::string swarm_name)
        : _swarm_name(std::move(swarm_name))
        , _infohash(util::sha1_digest(_swarm_name))
    { }

    std::expected<Ret, sys::error_code> get(Async yield) {
        // * Start a new job if one isn't already running
        // * Use previously returned result if it's not older than 5mins
        // * Otherwise wait for the running job to finish

        if (!_job || !_job->is_running()) {
            _job = make_job(yield.get_executor(), yield.log_path());
        }

        if (_last_result.is_fresh()) {
            return _last_result.value;
        }

#ifndef NDEBUG
        auto wd = watch_dog(
            yield.get_executor(),
            timeout_duration() + std::chrono::seconds(5),
            [&] {
                LOG_ERROR("PeerLookup::get failed to time out");
            }
        );
#endif

        compat([&](Cancel cancel, asio::yield_context yield) {
            _job->wait_for_finish(cancel, yield);
        })(yield);

        if (_last_result.ec) {
            return std::unexpected(_last_result.ec);
        } else {
            return _last_result.value;
        }
    }

    NodeID infohash() const {
        return _infohash;
    }

    const std::string& swarm_name() const {
        return _swarm_name;
    }

protected:
    // Children implement this to perform the actual peer lookup.
    virtual std::expected<Ret, sys::error_code> do_lookup(Async) = 0;

    // Used in log messages to identify the lookup strategy
    const char* _lookup_strategy_name = "Generic PeerLookup";

private:

    std::unique_ptr<Job> make_job(AsioExecutor exec, util::LogPath log_path) {
        auto job = std::make_unique<Job>(exec);

        job->start(
            [this, log_path = std::move(log_path)]
            (Cancel cancel, asio::yield_context y) mutable {
                Async yield(y, cancel, std::move(log_path));

                auto result = timeout(
                    timeout_duration(),
                    [&](Async yield) { return do_lookup(yield); },
                    yield
                );

                if (!result) {
                    if (result.error() == asio::error::timed_out) {
                        LOG_WARN(yield, _lookup_strategy_name, " lookup ",
                                        _infohash, " timed out");
                    }

                    return or_throw(yield.asio_yield(), result.error(), boost::none);
                }

                _last_result.ec = sys::error_code();
                _last_result.value = std::move(result).value();
                _last_result.time = Clock::now();

                return boost::none;
            }
        );

        return job;
    }

    std::string _swarm_name;
    NodeID _infohash;
    std::unique_ptr<Job> _job;
    Result _last_result;
};

}} // namespaces
