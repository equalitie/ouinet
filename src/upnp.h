#pragma once

#include <upnp.hpp>
#include <util/signal.h>
#include <async_sleep.h>

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

private:
    void loop( asio::executor exec
             , Cancel& cancel
             , asio::yield_context yield)
    {
        using namespace std;
        using namespace std::chrono;

        while (true)
        {
            auto r_igds = upnp::igd::discover(exec, yield);
            if (cancel) return;

            if (!r_igds) {
                async_sleep(exec, minutes(1), cancel, yield);
                if (cancel) return;
                continue;
            }

            auto igds = move(r_igds.value());

            for(auto& igd : igds) {
                // TODO: This shouldn't be a requirement and will probably not
                // work in many scenarios to require that the external port is
                // equal to the internal one.
                uint16_t external_port = _internal_port;
                auto r = igd.add_port_mapping( external_port
                                             , _internal_port
                                             , "Ouinet"
                                             , minutes(1)
                                             , yield);
                if (cancel) return;

                auto wait_time = [&] {
                    if (!r) return seconds(30);
                    return seconds(55);
                }();

                async_sleep(exec, wait_time, cancel, yield);
                if (cancel) return;
            }
        }
    }

private:
    Cancel _lifetime_cancel;
    uint16_t _internal_port;
};

} // namespace ouinet
