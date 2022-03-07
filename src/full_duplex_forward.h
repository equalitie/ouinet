#pragma once

#include <boost/asio/read.hpp>
#include "default_timeout.h"
#include "generic_stream.h"
#include "util/signal.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"

namespace ouinet {

// This assumes that there is no data already read from either connection,
// but pending send.  If there is, please send it beforehand.
template<class Stream1, class Stream2>
inline
void full_duplex(Stream1 c1, Stream2 c2, Cancel cancel, asio::yield_context yield)
{
    static const auto timeout = default_timeout::activity();

    static const auto half_duplex = []( auto& in
                                      , auto& out
                                      , auto& wdog
                                      , asio::yield_context& yield)
    {
        sys::error_code ec;
        std::array<uint8_t, 2048> data;

        for (;;) {
            size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
            if (ec) break;

            asio::async_write(out, asio::buffer(data, length), yield[ec]);
            if (ec) break;

            wdog.expires_after(timeout);
        }
        // On error, force the other half-duplex task to finish by closing both streams.
        // Otherwise, it will not notice until
        // (i) it reads and fails to write, or (ii) it times out on read.
        //
        // **Note:** This assumes that the other endoint wants
        // to shut both send & recv channels at roughly the same time.
        // We should look out for (esp. tunneled) protocols where this does not hold.
        if (ec) {
            in.close();
            out.close();
        }
    };

    auto cancel_slot = cancel.connect([&] { c1.close(); c2.close(); });

    auto wdog = watch_dog( c1.get_executor()
                         , timeout
                         , [&] { c1.close(); c2.close(); });

    WaitCondition wait_condition(c1.get_executor());

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(c1, c2, wdog, yield);
          });

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(c2, c1, wdog, yield);
          });

    wait_condition.wait(yield);
}

} // ouinet namespace
