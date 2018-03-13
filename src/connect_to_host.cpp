#include "connect_to_host.h"

#include "util.h"

#include <boost/asio/io_service.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

using namespace std;
using namespace ouinet;

GenericConnection
ouinet::connect_to_host( asio::io_service& ios
                       , beast::string_view host_and_port
                       , Signal<void()>& cancel_signal
                       , asio::yield_context yield)
{
    using namespace std;
    using tcp = asio::ip::tcp;

    auto hp = util::split_host_port(host_and_port);

    string host = hp.first .to_string();
    string port = hp.second.to_string();

    sys::error_code ec;

    tcp::resolver resolver{ios};
    auto cancel_lookup_slot = cancel_signal.connect([&resolver] {
        resolver.cancel();
    });

    // Look up the domain name
    auto const lookup = resolver.async_resolve({host, port}, yield[ec]);
    if (ec) return or_throw(yield, ec, GenericConnection());

    tcp::socket socket(ios);
    auto disconnect_slot = cancel_signal.connect([&socket] {
        socket.shutdown(tcp::socket::shutdown_both);
        socket.close();
    });

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return or_throw(yield, ec, GenericConnection());

    return GenericConnection(move(socket));
}

