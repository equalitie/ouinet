#pragma once

#include <boost/asio/read.hpp>
#include "generic_connection.h"
#include "util/wait_condition.h"

namespace ouinet {

inline
void full_duplex( GenericConnection& c1
                , GenericConnection& c2
                , asio::yield_context yield)
{
    static const auto half_duplex
        = []( GenericConnection& in
            , GenericConnection& out
            , asio::yield_context& yield)
    {
        sys::error_code ec;
        std::array<uint8_t, 2048> data;

        for (;;) {
            // XXX: Workaround: For some reason calling in.async_read_some
            // directly throws the boost::coroutines::detail::forced_unwind
            // exception from GenericConnection::async_read_some::result.get.
            // When it's used indirectly through async_read then it doesn't.
            //
            // ¯\_(ツ)_/¯
            //
            //// size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
            size_t length = asio::async_read
                ( in
                , asio::buffer(data)
                , [](const sys::error_code& ec, size_t size) -> size_t {
                    if (ec) return 0;
                    return size ? 0 : ~size_t(0);
                  }
                , yield[ec]);

            if (ec) break;

            asio::async_write(out, asio::buffer(data, length), yield[ec]);
            if (ec) break;
        }
    };

    assert(&c1.get_io_service() == &c2.get_io_service());

    WaitCondition wait_condition(c1.get_io_service());

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(c1, c2, yield);
          });

    asio::spawn
        ( yield
        , [&, lock = wait_condition.lock()](asio::yield_context yield) {
              half_duplex(c2, c1, yield);
          });

    wait_condition.wait(yield);
}

} // ouinet namespace
