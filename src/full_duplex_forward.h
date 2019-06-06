#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>

#include "default_timeout.h"
#include "generic_stream.h"
#include "util/wait_condition.h"
#include "util/watch_dog.h"

namespace ouinet {

static const size_t half_duplex_default_block = 2048;

// Low-level, one-direction operation.
// `already_read` bytes already present in `buffer` will be sent in the first batch.
template<class StreamIn, class StreamOut>
inline
void half_duplex( StreamIn& in, StreamOut& out
                , asio::mutable_buffer& buffer
                , size_t already_read, size_t max_transfer
                , WatchDog& wdog
                , asio::yield_context& yield)
{
    assert(max_transfer >= already_read);
    sys::error_code ec;

    while (max_transfer > 0) {
        auto buf = asio::buffer(buffer, max_transfer);
        size_t length = already_read + in.async_read_some(buf + already_read, yield[ec]);
        already_read = 0;  // only usable on first read
        if (ec) break;

        asio::async_write(out, asio::buffer(buf, length), yield[ec]);
        if (ec) break;

        max_transfer -= length;

        wdog.expires_after(default_timeout::half_duplex());
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
    return half_duplex( in, out, buffer, 0
                      , std::numeric_limits<std::size_t>::max()
                      , wdog, yield);
}

template<class StreamIn, class StreamOut>
inline
void half_duplex( StreamIn& in, StreamOut& out
                , asio::mutable_buffer& buffer
                , size_t already_read, size_t max_transfer
                , asio::yield_context yield)
{
    assert(&in.get_io_service() == &out.get_io_service());

    WatchDog wdog( in.get_io_service()
                 , default_timeout::half_duplex()
                 , [&] { in.close(); out.close(); });

    WaitCondition wait_condition(in.get_io_service());

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(in, out, buffer, already_read, max_transfer, wdog, yield);
          });

    wait_condition.wait(yield);
}

template<class StreamIn, class StreamOut>
inline
void half_duplex(StreamIn& in, StreamOut& out, asio::yield_context yield)
{
    std::array<uint8_t, half_duplex_default_block> data;
    auto buffer = asio::buffer(data);
    return half_duplex( in, out, buffer, 0
                      , std::numeric_limits<std::size_t>::max()
                      , yield);
}

template<class Stream1, class Stream2>
inline
void full_duplex(Stream1 c1, Stream2 c2, asio::yield_context yield)
{
    assert(&c1.get_io_service() == &c2.get_io_service());

    WatchDog wdog( c1.get_io_service()
                 , default_timeout::half_duplex()
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
