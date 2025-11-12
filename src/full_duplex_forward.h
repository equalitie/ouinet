#pragma once

#include <utility>

#include <boost/asio/read.hpp>
#include "default_timeout.h"
#include "generic_stream.h"
#include "or_throw.h"
#include "util/signal.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"
#include "util/yield.h"

namespace ouinet {

// This assumes that there is no data already read from either connection,
// but pending send.  If there is, please send it beforehand.
//
// A pair of counts is returned
// for bytes successfully forwarded (from `a` to `b`, from `b` to `a`).
template<class Stream1, class Stream2, class OnA2B, class OnB2A>
std::pair<std::size_t, std::size_t>
full_duplex( Stream1 a
           , Stream2 b
           , OnA2B on_a2b
           , OnB2A on_b2a
           , Cancel cancel
           , Yield yield)
{
    static const auto timeout = default_timeout::activity();

    static const auto half_duplex = []( auto& in
                                      , auto& out
                                      , auto& fwd_bytes_in_out
                                      , auto& on_transfer
                                      , auto& wdog
                                      , auto& cancel
                                      , asio::yield_context& yield)
    {
        sys::error_code ec;
        std::array<uint8_t, 2048> data;

        for (;;) {
            size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
            ec = compute_error_code(ec, cancel, wdog);
            if (ec) {
                break;
            }

            asio::async_write(out, asio::buffer(data, length), yield[ec]);
            ec = compute_error_code(ec, cancel, wdog);
            if (ec) {
                break;
            }

            fwd_bytes_in_out += length;  // the data was successfully forwarded
            on_transfer(length);
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

    auto cancel_slot = cancel.connect([&] { a.close(); b.close(); });

    auto wdog = watch_dog( a.get_executor()
                         , timeout
                         , [&] { a.close(); b.close(); });

    WaitCondition wait_condition(a.get_executor());
    std::size_t fwd_bytes_a2b = 0, fwd_bytes_b2a = 0;

    task::spawn_detached
        ( yield.get_executor()
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(a, b, fwd_bytes_a2b, on_a2b, wdog, cancel, yield);
          });

    task::spawn_detached
        ( yield.get_executor()
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(b, a, fwd_bytes_b2a, on_b2a, wdog, cancel, yield);
          });

    sys::error_code ec;
    wait_condition.wait(yield.native()[ec]);  // leave cancellation handling to tasks
    ec = compute_error_code(ec, cancel, wdog);

    return or_throw(yield, ec, std::make_pair(fwd_bytes_a2b, fwd_bytes_b2a));
}

template<class Stream1, class Stream2>
std::pair<std::size_t, std::size_t>
full_duplex(Stream1 a, Stream2 b, Cancel cancel, Yield yield)
{
    return full_duplex(
            std::move(a),
            std::move(b),
            [](size_t) {},
            [](size_t) {},
            std::move(cancel),
            std::move(yield));
}

} // ouinet namespace
