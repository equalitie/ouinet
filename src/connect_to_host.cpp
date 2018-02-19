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
                       , asio::yield_context yield)
{
    using namespace std;
    using tcp = asio::ip::tcp;

    auto hp = util::split_host_port(host_and_port);

    string host = hp.first .to_string();
    string port = hp.second.to_string();

    tcp::socket socket(ios);
    sys::error_code ec;

    tcp::resolver resolver{ios};

    // Look up the domain name
    auto const lookup = resolver.async_resolve({host, port}, yield[ec]);
    if (ec) return or_throw(yield, ec, GenericConnection(move(socket)));

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return or_throw(yield, ec, GenericConnection(move(socket)));

    return GenericConnection(move(socket));
}
