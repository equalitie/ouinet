#pragma once

#include <utility>

#include <boost/asio/read.hpp>
#include "default_timeout.h"
#include "generic_stream.h"
#include "or_throw.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"
#include "util/async.h"
#include "util/yield.h"

namespace ouinet {

class Cancel;

// This assumes that there is no data already read from either connection,
// but pending send.  If there is, please send it beforehand.
//
// A pair of counts is returned
// for bytes successfully forwarded (from `a` to `b`, from `b` to `a`).
template<class Stream1, class Stream2, class OnA2B, class OnB2A>
sys::error_code
full_duplex( Stream1 a
           , Stream2 b
           , OnA2B on_a2b
           , OnB2A on_b2a
           , Async yield)
{
    static const auto timeout = default_timeout::activity();

    sys::error_code ec;

    const auto half_duplex = [&ec]( auto& in
                                  , auto& out
                                  , auto& fwd_bytes_in_out
                                  , auto& on_transfer
                                  , auto& wdog
                                  , Async yield)
    {
        std::array<uint8_t, 2048> data;

        for (;;) {
            auto read_r = in.async_read_some(asio::buffer(data), yield);
            if (!wdog.is_running()) {
                if (!ec) ec = asio::error::timed_out;
                break;
            }
            if (!read_r.has_value()) {
                if (!ec) ec = read_r.error();
                break;
            }

            auto write_r = asio::async_write(out, asio::buffer(data, *read_r), yield);
            if (!wdog.is_running()) {
                if (!ec) ec = asio::error::timed_out;
                break;
            }
            if (!write_r.has_value()) {
                if (!ec) ec = write_r.error();
                break;
            }

            fwd_bytes_in_out += *read_r;  // the data was successfully forwarded
            on_transfer(*read_r);
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

    auto cancel_slot = yield.cancel_slot([&] { a.close(); b.close(); });

    auto wdog = watch_dog( a.get_executor()
                         , timeout
                         , [&] { a.close(); b.close(); });

    WaitCondition wait_condition(yield.get_executor());
    std::size_t fwd_bytes_a2b = 0, fwd_bytes_b2a = 0;

    yield.spawn(
        [&, lock = wait_condition.lock()](Async yield) {
            half_duplex(a, b, fwd_bytes_a2b, on_a2b, wdog, yield);
        });

    yield.spawn(
        [&, lock = wait_condition.lock()](Async yield) {
            half_duplex(b, a, fwd_bytes_b2a, on_b2a, wdog, yield);
        });

    [[maybe_unused]]
    auto _ = wait_condition.wait(yield);  // leave cancellation handling to tasks

    return ec;
}

template<class Stream1, class Stream2, class OnA2B, class OnB2A>
void
full_duplex( Stream1 a
           , Stream2 b
           , OnA2B on_a2b
           , OnB2A on_b2a
           , Cancel cancel, YieldContext yield)
{
    auto ec =
        full_duplex(
            std::move(a),
            std::move(b),
            std::move(on_a2b),
            std::move(on_b2a),
            Async(yield, cancel));

    return or_throw(yield, ec);
}

} // ouinet namespace
