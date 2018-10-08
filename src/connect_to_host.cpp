#include "connect_to_host.h"

#include "util.h"
#include "http_util.h"
#include "util/timeout.h"

#include <boost/asio/io_service.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

using namespace std;
using namespace ouinet;

using TCPLookup = asio::ip::tcp::resolver::results_type;


GenericConnection
ouinet::connect_to_host( asio::io_service& ios
                       , const string& host
                       , const string& port
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    sys::error_code ec;

    auto const lookup = util::tcp_async_resolve( host, port
                                               , ios, cancel_signal
                                               , yield[ec]);

    if (ec) return or_throw(yield, ec, GenericConnection());

    return connect_to_host(lookup, ios, cancel_signal, yield);
}

GenericConnection
ouinet::connect_to_host( const TCPLookup& lookup
                       , asio::io_service& ios
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    using tcp = asio::ip::tcp;

    sys::error_code ec;
    tcp::socket socket(ios);

    auto disconnect_slot = cancel_signal.connect([&socket] {
        sys::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    });

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return or_throw(yield, ec, GenericConnection());

    return GenericConnection(move(socket));
}

GenericConnection
ouinet::connect_to_host( const TCPLookup& lookup
                       , asio::io_service& ios
                       , std::chrono::steady_clock::duration timeout
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    return util::with_timeout
        ( ios
        , cancel_signal
        , timeout
        , [&] (auto& signal, auto yield) {
              return connect_to_host(lookup, ios, signal, yield);
          }
        , yield);
}
