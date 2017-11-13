#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>

#include "namespaces.h"
#include "fail.h"
#include "util.h"
#include "generic_connection.h"

namespace ouinet {

inline
std::unique_ptr<GenericConnection>
connect_to_host( asio::io_service& ios
               , beast::string_view host_and_port
               , sys::error_code& ec
               , asio::yield_context yield)
{
    using namespace std;
    using tcp = asio::ip::tcp;
    using Con = GenericConnectionImpl<tcp::socket>;

    auto hp = util::split_host_port(host_and_port);

    string host = hp.first .to_string();
    string port = hp.second.to_string();

    tcp::socket socket(ios);

    auto finish = [&socket] (auto ec, auto where) {
        fail(ec, where);
        return make_unique<Con>(move(socket));
    };

    tcp::resolver resolver{ios};

    // Look up the domain name
    auto const lookup = resolver.async_resolve({host, port}, yield[ec]);
    if (ec) return finish(ec, "resolve");

    // Make the connection on the IP address we get from a lookup
    asio::async_connect(socket, lookup, yield[ec]);
    if (ec) return finish(ec, "connect");

    return make_unique<Con>(move(socket));
}

} // ouinet namespace
