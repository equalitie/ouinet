#include "connect_to_host.h"

#include "http_util.h"
#include "or_throw.h"
#include "util/timeout.h"
#include "util/async.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

namespace ouinet {

using namespace std;
using tcp = asio::ip::tcp;

using TcpLookup = asio::ip::tcp::resolver::results_type;

tcp::socket
connect_to_host( const string& host
               , const uint16_t port
               , std::shared_ptr<dns::Resolver> dns_resolver
               , Cancel& cancel
               , asio::yield_context yield)
{
    sys::error_code ec;

    auto const lookup = dns_resolver->resolve( host, port
                                             , cancel
                                             , YieldContext(yield[ec]));
    return_or_throw_on_error(yield, cancel, ec, tcp::socket(yield.get_executor()));

    return connect_to_host(lookup, cancel, yield);
}

tcp::socket
connect_to_host( const TcpLookup& lookup
               , Cancel& cancel
               , asio::yield_context yield)
{
    auto ex = yield.get_executor();
    sys::error_code ec;
    tcp::socket socket(ex);

    auto disconnect_slot = cancel.connect([&socket] {
        sys::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    });

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, tcp::socket(ex));

    return socket;
}

std::expected<tcp::socket, sys::error_code>
connect_to_host(const TcpLookup& lookup, Async yield)
{
    tcp::socket socket(yield.get_executor());

    auto disconnect_slot = yield.cancel_slot([&socket] {
        sys::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    });

    // Make the connection on the IP address we get from a lookup
    if (auto r = asio::async_connect(socket, lookup, yield); !r) {
        return std::unexpected(r.error());
    }

    return socket;
}

tcp::socket
connect_to_host( const TcpLookup& lookup
               , std::chrono::steady_clock::duration timeout
               , Cancel& cancel
               , asio::yield_context yield)
{
    auto ex = yield.get_executor();

    return util::with_timeout
        ( ex
        , cancel
        , timeout
        , [&] (auto& cancel, auto yield) {
              return connect_to_host(lookup, cancel, yield);
          }
        , yield);
}

} // namespace
