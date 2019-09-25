#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "wait_condition.h"

#include "../namespaces.h"

namespace ouinet { namespace util {

static
std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(asio::io_service& ios, asio::yield_context yield)
{
    using namespace std;
    using tcp = asio::ip::tcp;
    using Ret = pair<tcp::socket, tcp::socket>;

    tcp::acceptor a(ios, tcp::endpoint(tcp::v4(), 0));
    tcp::socket s1(ios), s2(ios);

    sys::error_code accept_ec;
    sys::error_code connect_ec;

    WaitCondition wc(ios);

    asio::spawn(ios, [&, lock = wc.lock()] (asio::yield_context yield) mutable {
            a.async_accept(s2, yield[accept_ec]);
        });

    s1.async_connect(a.local_endpoint(), yield[connect_ec]);
    wc.wait(yield);

    if (accept_ec)  return or_throw(yield, accept_ec, Ret(move(s1),move(s2)));
    if (connect_ec) return or_throw(yield, connect_ec, Ret(move(s1),move(s2)));

    return make_pair(move(s1), move(s2));
}


}} // namespaces
