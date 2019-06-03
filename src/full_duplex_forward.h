#pragma once

#include <boost/asio/read.hpp>
#include "generic_stream.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"

namespace ouinet {

static const size_t half_duplex_default_block = 2048;
static const auto half_duplex_timeout = std::chrono::seconds(60);

// Low-level, one-direction operation.
// Data already present in `buffer` will be sent in the first batch.
template<class StreamIn, class StreamOut, class Buffer>
inline
void half_duplex( StreamIn& in, StreamOut& out
                , Buffer& buffer, size_t max_transfer
                , WatchDog& wdog
                , asio::yield_context& yield)
{
    sys::error_code ec;

    for (size_t cur_transfer = 0; cur_transfer < max_transfer;) {
        size_t length = in.async_read_some(buffer, yield[ec]);
        if (ec) break;

        asio::async_write(out, asio::buffer(buffer, length), yield[ec]);
        if (ec) break;

        cur_transfer += length;

        wdog.expires_after(half_duplex_timeout);
    }
}

template<class StreamIn, class StreamOut>
inline
void half_duplex( StreamIn& in, StreamOut& out
                , WatchDog& wdog
                , asio::yield_context& yield)
{
    std::array<uint8_t, half_duplex_default_block> data;
    auto buffer = asio::buffer(data);
    return half_duplex( in, out, buffer
                      , std::numeric_limits<std::size_t>::max()
                      , wdog, yield);
}

template<class StreamIn, class StreamOut>
inline
void half_duplex( StreamIn& in, StreamOut& out, size_t max_transfer
                , asio::yield_context yield)
{
    assert(&in.get_io_service() == &out.get_io_service());

    WatchDog wdog( in.get_io_service()
                 , half_duplex_timeout
                 , [&] { in.close(); out.close(); });

    WaitCondition wait_condition(in.get_io_service());

    std::array<uint8_t, half_duplex_default_block> data;
    auto buffer = asio::buffer(data);

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(in, out, buffer, max_transfer, wdog, yield);
          });

    wait_condition.wait(yield);
}

template<class StreamIn, class StreamOut>
inline
void half_duplex(StreamIn& in, StreamOut& out, asio::yield_context yield)
{
    return half_duplex( in, out, std::numeric_limits<std::size_t>::max()
                      , yield);
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
