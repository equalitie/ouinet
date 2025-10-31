#define BOOST_TEST_MODULE injector_resolver
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>
//#include <boost/beast/message.hpp>

#include "../src/injector.h"

using namespace ouinet;
using Request = http::request<http::string_body>;

static const string public_host[] = {
    "ouinet.work",
    "ceno.app",
    "example.com",
    "9.9.9.9",
    "172.15.0.1",
    "172.32.0.1",
    "192.167.8.4",
    "192.169.7.5",
};
BOOST_DATA_TEST_CASE(test_resolve_target_public,
                     boost::unit_test::data::make(public_host),
                     hostname)
{
    asio::io_context ctx;
    Cancel cancel;
    task::spawn_detached(ctx, [&](asio::yield_context yield)
    {
        Request req;
        req.set(http::field::host, hostname);
        Yield y(ctx.get_executor(), std::move(yield), "PUBLIC");
        BOOST_CHECK_NO_THROW(resolve_target(req, false, ctx.get_executor(), cancel, y));
    });
    ctx.run();
}

static const string loopback_host[] = {
    // ipv4
    "localhost",
    "host.localdomain",
    "127.0.0.1",
    "127.1.2.3",
    // ipv6
    "ip6-localhost",
    "ip6-loopback",
    "::1:8080",
    "::ffff:127.0.0.1:8080",
    "::127.0.0.1:8080",
};
BOOST_DATA_TEST_CASE(test_resolve_target_loopback,
                     boost::unit_test::data::make(loopback_host),
                     hostname)
{
    asio::io_context ctx;
    Cancel cancel;
    task::spawn_detached(ctx, [&](asio::yield_context yield)
    {
        Request req;
        req.set(http::field::host, hostname);
        Yield y(ctx.get_executor(), std::move(yield), "LOOPBACK");
        BOOST_CHECK_THROW(
            resolve_target(req, false, ctx.get_executor(), cancel, y),
            boost::system::system_error);
    });
    ctx.run();
}

static const string private_host[] = {
    // ipv4
    "192.168.0.1",
    "172.17.0.1",
    "10.4.2.1",
    // ipv6
    "::ffff:192.168.1.1:8080",
    "::ffff:172.17.0.1:8080",
    "::ffff:10.4.2.1:8080",
    "::192.168.1.1:8080",
    "::172.17.0.1:8080",
    "::10.4.2.1:8080",
};
BOOST_DATA_TEST_CASE(test_resolve_target_restrict_private,
                     boost::unit_test::data::make(private_host),
                     hostname)
{
    asio::io_context ctx;
    Cancel cancel;
    task::spawn_detached(ctx, [&](asio::yield_context yield)
    {
        Request req;
        req.set(http::field::host, hostname);
        Yield y(ctx.get_executor(), std::move(yield), "PRIVATE");
        BOOST_CHECK_THROW(
            resolve_target(req, false, ctx.get_executor(), cancel, y),
            boost::system::system_error);
    });
    ctx.run();
}

BOOST_DATA_TEST_CASE(test_resolve_target_allow_private,
                     boost::unit_test::data::make(private_host),
                     hostname)
{
    asio::io_context ctx;
    Cancel cancel;
    task::spawn_detached(ctx, [&](asio::yield_context yield)
    {
        Request req;
        req.set(http::field::host, hostname);
        Yield y(ctx.get_executor(), std::move(yield), "PRIVATE");
        BOOST_CHECK_NO_THROW(resolve_target(req, true, ctx.get_executor(), cancel, y));
    });
    ctx.run();
}
