#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include "fail.h"
#include "or_throw.h"
#include "generic_connection.h"
#include "util/signal.h"

namespace ouinet {

template<class RequestType>
inline
http::response<http::dynamic_body>
fetch_http_page( asio::io_service& ios
               , GenericConnection& con
               , RequestType req
               , Signal<void()>& abort_signal
               , asio::yield_context yield)
{
    http::response<http::dynamic_body> res;

    sys::error_code ec;

    auto close_con_slot = abort_signal.connect([&con] {
        con.close();
    });

    // Send the HTTP request to the remote host
    http::async_write(con, req, yield[ec]);

    // Ignore end_of_stream error, there may still be data in the receive
    // buffer we can read.
    if (ec == http::error::end_of_stream) {
        ec = sys::error_code();
    }

    if (ec) return or_throw(yield, ec, move(res));

    beast::flat_buffer buffer;

    // Receive the HTTP response
    http::async_read(con, buffer, res, yield[ec]);

    return or_throw(yield, ec, move(res));
}

}
