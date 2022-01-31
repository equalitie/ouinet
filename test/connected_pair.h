#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "../src/util/wait_condition.h"
#include "../src/util/yield.h"
#include "../src/namespaces.h"

namespace ouinet { namespace util {

inline
std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(const asio::executor& ex, asio::yield_context yield)
{
    using namespace std;
    using tcp = asio::ip::tcp;
    using Ret = pair<tcp::socket, tcp::socket>;

    tcp::acceptor a(ex, tcp::endpoint(tcp::v4(), 0));
    tcp::socket s1(ex), s2(ex);

    sys::error_code accept_ec;
    sys::error_code connect_ec;

    WaitCondition wc(ex);

    asio::spawn(ex, [&, lock = wc.lock()] (asio::yield_context yield) mutable {
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
connected_pair(asio::io_context& ctx, asio::yield_context yield)
{
    return connected_pair(ctx.get_executor(), yield);
}

inline
std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(const asio::executor& ex, Yield yield)
{
    return connected_pair(ex, static_cast<asio::yield_context>(yield));
}

inline
std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(asio::io_context& ctx, Yield yield)
{
    return connected_pair(ctx, static_cast<asio::yield_context>(yield));
}

}} // namespaces
