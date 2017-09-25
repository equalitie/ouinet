#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

#include "namespaces.h"
#include "fail.h"

namespace ouinet {

inline
std::pair< boost::beast::string_view
         , boost::beast::string_view
         >
split_host_port(const boost::beast::string_view& hp)
{
    using namespace std;

    auto pos = hp.find(':');

    if (pos == string::npos) {
        return make_pair(hp, "80");
    }

    return make_pair(hp.substr(0, pos), hp.substr(pos+1));
}

inline
asio::ip::tcp::socket
connect_to_host( asio::io_service& ios
               , beast::string_view host_and_port
               , sys::error_code& ec
               , asio::yield_context yield)
{
    using namespace std;
    using tcp = asio::ip::tcp;

    auto hp = split_host_port(host_and_port);

    string host = hp.first .to_string();
    string port = hp.second.to_string();

    tcp::socket socket(ios);

    auto finish = [&socket] (auto ec, auto where) {
        fail(ec, where);
        return move(socket);
    };

    tcp::resolver resolver{ios};

    // Look up the domain name
    auto const lookup = resolver.async_resolve({host, port}, yield[ec]);
    if (ec) return finish(ec, "resolve");

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return finish(ec, "connect");

    return socket;
}

} // ouinet namespace
