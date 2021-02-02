#pragma once

#include <boost/format.hpp>
#include <boost/optional.hpp>

#include <upnp.h>
#include <util/random.h>
#include <util/signal.h>
#include <async_sleep.h>
#include <defer.h>
#include "util/handler_tracker.h"

namespace ouinet {

class UPnPUpdater {
public:
    UPnPUpdater( asio::executor exec
               , uint16_t external_port
               , uint16_t internal_port)
        : _external_port(external_port)
        , _internal_port(internal_port)
        , _random_id(util::random::number<uint16_t>())
    {
        TRACK_SPAWN(exec, ([
            this,
            exec,
            c = _lifetime_cancel
        ] (asio::yield_context yield) mutable {
            while (!c) {
                try {
                    loop(exec, c, yield);
                } catch (const std::exception& e) {
                    if (!c) {
                        LOG_WARN("UPnP Loop has thrown an exception, will restart in 5s");
                    }
                }
                async_sleep(exec, std::chrono::seconds(5), c, yield);
            }
        }));
    }

    ~UPnPUpdater() {
        _lifetime_cancel();
    }

    bool mapping_is_active() const {
        return _mapping_is_active;
    }

private:
    void loop( asio::executor exec
             , Cancel& cancel
             , asio::yield_context yield)
    {
        using namespace std;
        using namespace std::chrono;

        auto on_exit = defer([&] {
            if (cancel) return;
            mapping_disabled();
        });

        static const auto lease_duration    = minutes(3);
        static const auto success_wait_time = lease_duration - seconds(10);
        static const auto failure_wait_time = minutes(1);
        static const auto recent_margin     = seconds(10);  // max RPC round-trip time
        static const auto timeout_pause     = seconds(1);  // to ensure mapping removal after timeout

        auto mapping_desc = (boost::format("Ouinet-%04x") % _random_id).str();

        while (true)
        {
            auto round_begin = steady_clock::now();

            auto r_igds = upnp::igd::discover(exec, yield);
            if (cancel) return;

            if (!r_igds) {
                mapping_disabled();
                LOG_DEBUG("UPnP: No IGDs found, waiting");
                async_sleep(exec, failure_wait_time, cancel, yield);
                if (cancel) return;
                continue;
            }

            auto igds = move(r_igds.value());

            LOG_DEBUG("UPnP: Adding mappings for \"", mapping_desc, "\"...");
            size_t success_cnt = 0;
            boost::optional<steady_clock::time_point> earlier_buggy_timeout;
            for(auto& igd : igds) {
                auto cancelled = cancel.connect([&] { igd.stop(); });

                auto r = igd.add_port_mapping( upnp::igd::udp
                                             , _external_port
                                             , _internal_port
                                             , mapping_desc
                                             , lease_duration
                                             , yield);
                if (cancel) return;
                if (!r) continue;

                auto query_begin = steady_clock::now();
                auto curr_duration = get_mapping_duration(igd, mapping_desc, cancel, yield);
                if (!curr_duration || lease_duration >= *curr_duration + recent_margin) {
                    // Versions of MiniUPnPd before 2015-07-09 fail to refresh existing mappings,
                    // see <https://github.com/miniupnp/miniupnp/issues/131>,
                    // so check actual result and do not count if failed.
                    LOG_VERBOSE("UPnP: IGD did not add/refresh mapping for \"", mapping_desc, "\""
                                " but reported no error; buggy IGD/router?");
                    auto mapping_timeout = query_begin + (curr_duration ? *curr_duration : seconds(0));
                    if (!earlier_buggy_timeout || mapping_timeout < *earlier_buggy_timeout)
                        earlier_buggy_timeout = mapping_timeout;
                    continue;
                }
                LOG_DEBUG("UPnP: Successfully added/refreshed one mapping");
                success_cnt++;
                mapping_enabled();
            }
            LOG_DEBUG("UPnP: Adding mappings for \"", mapping_desc, "\": done");

            if (success_cnt == 0 && !earlier_buggy_timeout) mapping_disabled();

            auto wait_time = [&] () -> seconds {
                if (success_cnt == 0) {
                    // Wait until the oldest mapping times out to re-add them,
                    // but only if we just got to add mappings to buggy IGDs.
                    if (!earlier_buggy_timeout) return failure_wait_time;
                    auto now = steady_clock::now();
                    if (*earlier_buggy_timeout <= now) return timeout_pause;
                    return duration_cast<seconds>(*earlier_buggy_timeout - now + timeout_pause);
                }
                // Wait until a little before mappings would time out to refresh them.
                auto round_elapsed = steady_clock::now() - round_begin;
                if (round_elapsed >= success_wait_time) return seconds(0);
                return success_wait_time - duration_cast<seconds>(round_elapsed);
            }();

            async_sleep(exec, wait_time, cancel, yield);
            if (cancel) return;
        }
    }

    void mapping_enabled() {
        if (!_mapping_is_active) {
            LOG_INFO("UPnP: Mapping enabled for UDP EXT_PORT:", _external_port
                    , " INT_PORT:", _internal_port);
        }
        _mapping_is_active = true;
    }
    void mapping_disabled() {
        if (_mapping_is_active) {
            LOG_WARN("UPnP: Mapping disabled");
        }
        _mapping_is_active = false;
    }

    boost::optional<std::chrono::seconds>
    get_mapping_duration( upnp::igd& igd, const std::string& desc
                        , Cancel& cancel, asio::yield_context yield) const
    {
        auto cancelled = cancel.connect([&] { igd.stop(); });
        auto r_mappings = igd.get_list_of_port_mappings( upnp::igd::udp
                                                       , _external_port, _external_port, 1
                                                       , yield);
        if (cancel) return {};
        if (!r_mappings) return {};

        for (const auto& m : r_mappings.value())
            if ( m.enabled && _internal_port == m.int_port && desc == m.description)
                return m.lease_duration;

        return {};
    }

private:
    Cancel _lifetime_cancel;
    uint16_t _external_port;
    uint16_t _internal_port;
    // The desciption for mappings includes a random value
    // to ease tracking those added by this UPnP client.
    // Probably not the most secure option but simple enough
    // without having to check our own address (which is probaly unreliable).
    uint16_t _random_id;
    bool _mapping_is_active = false;
};

} // namespace ouinet
