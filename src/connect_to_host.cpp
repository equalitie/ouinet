#include "connect_to_host.h"

#include "util.h"
#include "http_util.h"
#include "util/timeout.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

using namespace std;
using namespace ouinet;
using tcp = asio::ip::tcp;

using TcpLookup = asio::ip::tcp::resolver::results_type;


tcp::socket
ouinet::connect_to_host( const asio::executor& ex
                       , const string& host
                       , const string& port
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    sys::error_code ec;

    auto const lookup = util::tcp_async_resolve( host, port
                                               , ex, cancel_signal
                                               , yield[ec]);

    if (ec) return or_throw(yield, ec, tcp::socket(ex));

    return connect_to_host(lookup, ex, cancel_signal, yield);
}

tcp::socket
ouinet::connect_to_host( const TcpLookup& lookup
                       , const asio::executor& ex
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    sys::error_code ec;
    tcp::socket socket(ex);

    auto disconnect_slot = cancel_signal.connect([&socket] {
        sys::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    });

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return or_throw(yield, ec, tcp::socket(ex));

    return socket;
}

tcp::socket
ouinet::connect_to_host( const TcpLookup& lookup
                       , const asio::executor& ex
                       , std::chrono::steady_clock::duration timeout
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    return util::with_timeout
        ( ex
        , cancel_signal
        , timeout
        , [&] (auto& signal, auto yield) {
              return connect_to_host(lookup, ex, signal, yield);
          }
        , yield);
}
