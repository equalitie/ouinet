#pragma once

#include <vector>

#include <boost/asio/ip/udp.hpp>
#include <boost/format.hpp>
#include <boost/optional.hpp>

#include <upnp.h>
#include <util/random.h>
#include <util/signal.h>
#include <util/str.h>
#include <async_sleep.h>
#include <defer.h>
#include "util/handler_tracker.h"

namespace ouinet {

class UPnPUpdater {
public:
    using UdpEndpoint = asio::ip::udp::endpoint;
    using UdpEndpoints = std::vector<UdpEndpoint>;
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

    bool is_available() const {
        return _is_available;
    }

    bool mapping_is_active() const {
        return _mapping_is_active;
    }

    UdpEndpoints get_external_endpoints() const {
        if (!_external_endpoints) return {};
        return *_external_endpoints;
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
                _is_available = false;
                mapping_disabled();
                LOG_DEBUG("UPnP: No IGDs found, waiting");
                async_sleep(exec, failure_wait_time, cancel, yield);
                if (cancel) return;
                continue;
            }
            _is_available = true;

            auto igds = move(r_igds.value());

            LOG_DEBUG("UPnP: Setting mappings for \"", mapping_desc, "\"...");
            auto int_addr = util::get_local_ipv4_address();
            size_t success_cnt = 0;
            auto ext_eps = std::make_unique<UdpEndpoints>();
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
                if (!r) continue;  // failure, no buggy timeout

                auto query_begin = steady_clock::now();
                auto curr_duration = get_mapping_duration(igd, mapping_desc, int_addr, cancel, yield);
                if (!curr_duration) {
                    LOG_WARN("UPnP: IGD \"", igd.friendly_name(), "\""
                             " did not set mapping \"", mapping_desc, "\""
                             " but reported no error; buggy IGD/router?");
                    continue;  // failure, no buggy timeout
                }
                if (lease_duration >= *curr_duration + recent_margin) {
                    // Versions of MiniUPnPd before 2015-07-09 fail to update existing mappings,
                    // see <https://github.com/miniupnp/miniupnp/issues/131>,
                    // so check actual result and do not count if failed.
                    LOG_WARN("UPnP: IGD \"", igd.friendly_name(), "\""
                             " did not update mapping \"", mapping_desc, "\""
                             " but reported no error; buggy IGD/router?"
                             " (duration=", curr_duration->count(), "s)");
                    auto mapping_timeout = query_begin + *curr_duration;
                    if (!earlier_buggy_timeout || mapping_timeout < *earlier_buggy_timeout)
                        earlier_buggy_timeout = mapping_timeout;
                    continue;  // buggy timeout
                }
                if ( *curr_duration > lease_duration
                      // Zero duration indicates a static port mapping,
                      // which should not happen for an entry created by the client.
                   || curr_duration->count() == 0) {
                    LOG_WARN("UPnP: IGD \"", igd.friendly_name(), "\""
                             " reports excessive mapping lease duration"
                             " (", curr_duration->count(), "s)");
                    // The mapping is ours and it should be operational, though,
                    // so proceed.
                }
                LOG_DEBUG("UPnP: Successfully added/updated one mapping");
                success_cnt++;

                // Note down the external endpoint for status repoting.
                auto r_ext_ep = igd.get_external_address(yield);
                if (r_ext_ep)
                    ext_eps->push_back(UdpEndpoint(r_ext_ep.value(), _external_port));
                else
                    LOG_WARN("UPnP: Failed to get external address"
                             " from IGD \"", igd.friendly_name(), "\": ", r_ext_ep.error());

                mapping_enabled();
            }
            _external_endpoints = move(ext_eps);
            LOG_DEBUG("UPnP: Setting mappings for \"", mapping_desc, "\": done");

            if (success_cnt == 0 && !earlier_buggy_timeout) mapping_disabled();

            auto wait_time = [&] () -> seconds {
                if (success_cnt == 0) {
                    // Wait until the oldest mapping times out to re-add them,
                    // but only if we just got to add mappings to buggy IGDs.
                    if (!earlier_buggy_timeout) return failure_wait_time;
                    auto now = steady_clock::now();
                    auto buggy_update = *earlier_buggy_timeout + timeout_pause;
                    if (buggy_update < now) return seconds(0);
                    return [] (auto d) {  // std::chrono::ceil not in c++1z
                        auto t = duration_cast<seconds>(d);
                        return t + seconds(t < d ? 1 : 0);
                    }(buggy_update - now);
                }
                // Wait until a little before mappings would time out to update them.
                auto round_elapsed = steady_clock::now() - round_begin;
                if (round_elapsed >= success_wait_time) return seconds(0);
                return duration_cast<seconds>(success_wait_time - round_elapsed);
            }();

            async_sleep(exec, wait_time, cancel, yield);
            if (cancel) return;
        }
    }

    void mapping_enabled() {
        if (!_mapping_is_active) {
            LOG_INFO("UPnP: Mapping enabled for UDP; ext_port=", _external_port
                    , " int_port=", _internal_port);
        }
        _mapping_is_active = true;
    }
    void mapping_disabled() {
        if (_mapping_is_active) {
            LOG_WARN("UPnP: Mapping disabled");
        }
        _external_endpoints = nullptr;
        _mapping_is_active = false;
    }

    boost::optional<std::chrono::seconds>
    get_mapping_duration( upnp::igd& igd, const std::string& desc
                        , const boost::optional<asio::ip::address>& int_addr
                        , Cancel& cancel, asio::yield_context yield) const
    {
        auto cancelled = cancel.connect([&] { igd.stop(); });

        // `igd.get_list_of_port_mappings` is more convenient,
        // but that requires full IGDv2 support, so stick to IGDv1 operations.
        uint16_t stale_ext_port = 0;
        for (uint16_t index = 0; ; ++index) {
            auto r_mapping = igd.get_generic_port_mapping_entry(index, yield);
            if (cancel || !r_mapping) break;  // no more port mappings, or error
            const auto& m = r_mapping.value();

            if (m.proto != upnp::igd::udp) continue;
            if ( m.enabled
               && _external_port == m.ext_port && _internal_port == m.int_port
               && desc == m.description)
                return m.lease_duration;
            if ( int_addr
               && *int_addr == m.int_client && _internal_port == m.int_port
               && desc != m.description) {
                LOG_VERBOSE("UPnP: IGD \"", igd.friendly_name(), "\""
                            " has stale mapping \"", m.description, "\""
                            " for external port ", m.ext_port,
                            " with our same local UDP endpoint"
                            " and duration=", m.lease_duration.count(), "s");
                stale_ext_port = m.ext_port;
            }
        }
        // Cleanup stale mapping (only one can be removed with IGD interface).
        if (stale_ext_port != 0) {
            auto r = igd.delete_port_mapping(upnp::igd::udp, stale_ext_port, yield);
            if (r)
                LOG_VERBOSE("UPnP: IGD \"", igd.friendly_name(), "\""
                            " successfully removed stale mapping"
                            " for external port ", stale_ext_port);
            else
                LOG_WARN("UPnP: IGD \"", igd.friendly_name(), "\""
                         " failed to remove stale mapping"
                         " for external port ", stale_ext_port, ": ", r.error());
        }
        return boost::none;
    }

private:
    Cancel _lifetime_cancel;
    uint16_t _external_port;
    uint16_t _internal_port;
    std::unique_ptr<UdpEndpoints> _external_endpoints;
    // The desciption for mappings includes a random value
    // to ease tracking those added by this UPnP client.
    // Probably not the most secure option but simple enough
    // without having to check our own address (which is probaly unreliable).
    uint16_t _random_id;
    bool _mapping_is_active = false;
    bool _is_available = false;
};

} // namespace ouinet
