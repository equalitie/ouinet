#pragma once

#include <upnp.h>
#include <util/signal.h>
#include <async_sleep.h>
#include <defer.h>

namespace ouinet {

class UPnPUpdater {
public:
    UPnPUpdater(asio::executor exec, uint16_t internal_port)
        : _internal_port(internal_port)
    {
        asio::spawn(exec, [this, exec, c = _lifetime_cancel
        ] (asio::yield_context yield) mutable {
            loop(exec, c, yield);
        });
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

        auto on_exit = defer([&] ({
            if (cancel) return;
            _mapping_is_active = false;
        });

        while (true)
        {
            auto r_igds = upnp::igd::discover(exec, yield);
            if (cancel) return;

            if (!r_igds) {
                _mapping_is_active = false;
                async_sleep(exec, minutes(1), cancel, yield);
                if (cancel) return;
                continue;
            }

            auto igds = move(r_igds.value());

            size_t success_cnt = 0;
            for(auto& igd : igds) {
                auto cancelled = cancel.connect([&] { igd.stop(); });

                // TODO: This shouldn't be a requirement and will probably not
                // work in many scenarios to require that the external port is
                // equal to the internal one.
                uint16_t external_port = _internal_port;
                auto r = igd.add_port_mapping( upnp::igd::udp
                                             , external_port
                                             , _internal_port
                                             , "Ouinet"
                                             , minutes(1)
                                             , yield);
                if (cancel) return;
                if (r) {
                    success_cnt++;
                    _mapping_is_active = true;
                }
            }

            if (success_cnt == 0) _mapping_is_active false;

            auto wait_time = [&] {
                if (!r) return seconds(30);
                return seconds(55);
            }();

            async_sleep(exec, wait_time, cancel, yield);
            if (cancel) return;
        }
    }

private:
    Cancel _lifetime_cancel;
    uint16_t _internal_port;
    bool _mapping_is_active = false;
};

} // namespace ouinet
