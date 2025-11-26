#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "../src/util/wait_condition.h"
#include "../src/util/yield.h"
#include "../src/namespaces.h"

namespace ouinet { namespace util {

inline
std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(asio::yield_context yield)
{
    auto ex = yield.get_executor();

    using namespace std;
    using tcp = asio::ip::tcp;
    using Ret = pair<tcp::socket, tcp::socket>;

    auto loopback_ep = tcp::endpoint(asio::ip::address_v4::loopback(), 0);
    tcp::acceptor a(ex, loopback_ep);
    tcp::socket s1(ex), s2(ex);

    sys::error_code accept_ec;
    sys::error_code connect_ec;

    WaitCondition wc(ex);

    task::spawn_detached(ex, [&, lock = wc.lock()] (asio::yield_context yield) mutable {
            a.async_accept(s2, yield[accept_ec]);
        });

    s1.async_connect(a.local_endpoint(), yield[connect_ec]);
    wc.wait(yield);

    if (accept_ec)  return or_throw(yield, accept_ec, Ret(move(s1),move(s2)));
    if (connect_ec) return or_throw(yield, connect_ec, Ret(move(s1),move(s2)));

    return make_pair(move(s1), move(s2));
}

inline
std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(YieldContext yield)
{
    return connected_pair(yield.native());
}

}} // namespaces
