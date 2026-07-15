#include "connect_to_host.h"

#include "http_util.h"
#include "or_throw.h"
#include "util/async.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

namespace ouinet {

using namespace std;
using tcp = asio::ip::tcp;

using TcpLookup = asio::ip::tcp::resolver::results_type;

std::expected<tcp::socket, sys::error_code>
connect_to_host( const string& host
               , const uint16_t port
               , std::shared_ptr<dns::Resolver> dns_resolver
               , Async yield)
{
    auto const lookup = dns_resolver->resolve(host, port, yield);

    if (!lookup) return std::unexpected(lookup.error());

    return connect_to_host(std::move(*lookup), yield);
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

std::expected<tcp::socket, sys::error_code>
connect_to_host( const TcpLookup& lookup
               , std::chrono::steady_clock::duration timeout
               , Async yield)
{
    auto y = yield;
    auto wd = watch_dog(y.get_executor(), timeout, [&] { y.cancel(); });

    try {
        return connect_to_host(lookup, y);
    }
    catch (Async::Cancelled const&) {
        if (yield.is_cancelled()) throw;
        return std::unexpected(asio::error::timed_out);
    }
}

} // namespace
