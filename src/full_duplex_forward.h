#pragma once

#include <boost/asio/read.hpp>
#include "generic_stream.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"

namespace ouinet {

static const auto half_duplex_timeout = std::chrono::seconds(60);

// Low-level, one-direction operation.
template<class StreamIn, class StreamOut>
inline
void half_duplex( StreamIn& in, StreamOut& out
                , WatchDog& wdog
                , asio::yield_context& yield)
{
    sys::error_code ec;
    std::array<uint8_t, 2048> data;

    for (;;) {
        size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
        if (ec) break;

        asio::async_write(out, asio::buffer(data, length), yield[ec]);
        if (ec) break;

        wdog.expires_after(half_duplex_timeout);
    }
}

template<class Stream1, class Stream2>
inline
void full_duplex(Stream1 c1, Stream2 c2, asio::yield_context yield)
{
    assert(&c1.get_io_service() == &c2.get_io_service());

    WatchDog wdog( c1.get_io_service()
                 , half_duplex_timeout
                 , [&] { c1.close(); c2.close(); });

    WaitCondition wait_condition(c1.get_io_service());

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
