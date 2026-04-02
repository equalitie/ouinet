#pragma once

#include <set>
#include <string>
#include <util/async_job.h>
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

    static Clock::duration timeout() {
#ifndef NDEBUG // debug
        return std::chrono::minutes(1);
#else // release
        return std::chrono::minutes(3);
#endif
    }

public:
    PeerLookup(PeerLookup&&) = delete;

    PeerLookup(std::string swarm_name, AsioExecutor exec)
        : _swarm_name(std::move(swarm_name))
        , _infohash(util::sha1_digest(_swarm_name))
        , _exec(exec)
        , _cv(_exec)
    { }

    Ret get(Cancel c, asio::yield_context y) {
        // * Start a new job if one isn't already running
        // * Use previously returned result if it's not older than 5mins
        // * Otherwise wait for the running job to finish

        auto cancel_con = _lifetime_cancel.connect([&] { c(); });

        if (!_job) {
            _job = make_job();
        }

        if (_last_result.is_fresh()) {
            return _last_result.value;
        }

#ifndef NDEBUG
        auto wd = watch_dog(_exec, timeout() + std::chrono::seconds(5), [&] {
                LOG_ERROR("PeerLookup::get failed to time out");
            });
#endif

        sys::error_code ec;
        _cv.wait(c, y[ec]);

        return_or_throw_on_error(y, c, ec, Ret{});

        // (ec == operation_aborted) implies (c == true)
        assert(_last_result.ec != asio::error::operation_aborted || c);

        return or_throw(y, _last_result.ec, _last_result.value);
    }

    virtual ~PeerLookup() { _lifetime_cancel(); }

    NodeID infohash() const {
        return _infohash;
    }

    const std::string& swarm_name() const {
        return _swarm_name;
    }

protected:
    // Children implement this to perform the actual peer lookup.
    virtual Ret do_lookup(Cancel& c, asio::yield_context y) = 0;

    // Used in log messages to identify the lookup strategy
    const char* _lookup_strategy_name = "Generic PeerLookup";

private:

    std::unique_ptr<Job> make_job() {
        auto job = std::make_unique<Job>(_exec);

        job->start([ self = this
                   , lc = std::make_shared<Cancel>(_lifetime_cancel)
                   ] (Cancel c, asio::yield_context y) mutable {
            auto cancel_con = lc->connect([&] { c(); });

            auto on_exit = defer([&] {
                    if (*lc) return;
                    self->_cv.notify();
                    self->_job = nullptr;
                });

            auto wd = watch_dog(self->_exec, timeout(), [&] {
                    LOG_WARN(self->_lookup_strategy_name, " lookup ",
                             self->_infohash, " timed out");
                    c();
                });

            sys::error_code ec;

            auto result = self->do_lookup(c, y[ec]);

            if (!c && !ec) {
                self->_last_result.ec    = ec;
                self->_last_result.value = std::move(result);
                self->_last_result.time  = Clock::now();
            }

            return or_throw(y, ec, boost::none);
        });

        return job;
    }

    std::string _swarm_name;
    NodeID _infohash;
    AsioExecutor _exec;
    std::unique_ptr<Job> _job;
    ConditionVariable _cv;
    Result _last_result;
    Cancel _lifetime_cancel;
};

}} // namespaces
