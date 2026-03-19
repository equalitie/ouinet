#define BOOST_TEST_MODULE injector_resolver
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>
//#include <boost/beast/message.hpp>

#include "../src/injector.h"

using namespace ouinet;
using Request = http::request<http::string_body>;

static const std::string public_host[] = {
    "ouinet.work",
    "ceno.app",
    "example.com",
    "9.9.9.9",
    " 169.252.251.250",
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
        bool allow_private_targets = false;
        Request req;
        req.set(http::field::host, hostname);
        YieldContext y(std::move(yield), util::LogPath("PUBLIC"));
        BOOST_CHECK_NO_THROW(resolve_target( req
                                           , allow_private_targets, std::make_shared<dns::Resolver>()
                                           , ctx.get_executor(), cancel, y));
    });
    ctx.run();
}

static const std::string loopback_host[] = {
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
        bool allow_private_targets = false;
        Request req;
        req.set(http::field::host, hostname);
        YieldContext y(std::move(yield), util::LogPath("LOOPBACK"));
        BOOST_CHECK_THROW(
            resolve_target( req
                          , allow_private_targets
                          , std::make_shared<dns::Resolver>()
                          , ctx.get_executor()
                          , cancel, y),
            boost::system::system_error);
    });
    ctx.run();
}

static const std::string private_host[] = {
    // ipv4
    "192.168.0.1",
    "169.254.10.5",
    "172.17.0.1",
    "10.4.2.1",
    // ipv6 link-local
    "[fe80::1]",
    "[fe80:0::1]",
    "[fe80:0:0::1]",
    "[fe80:0:0:0::1]",
    // ipv6 unique-local
    "[fc00::1]",
    "[fc00:0::1]",
    "[fc00:0:0::1]",
    "[fc00:0:0:0::1]",
    "[fd00::1]",
    "[fd00:0::1]",
    "[fd00:0:0::1]",
    "[fd00:0:0:0::1]",
    // ipv6 mapped ipv4 addresses
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
        bool allow_private_targets = false;
        Request req;
        req.set(http::field::host, hostname);
        YieldContext y(std::move(yield), util::LogPath("PRIVATE"));
        BOOST_CHECK_THROW(
            resolve_target( req
                          , allow_private_targets
                          , std::make_shared<dns::Resolver>()
                          , ctx.get_executor()
                          , cancel, y),
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
        bool allow_private_targets = true;
        Request req;
        req.set(http::field::host, hostname);
        YieldContext y(std::move(yield), util::LogPath("PRIVATE"));
        BOOST_CHECK_NO_THROW(resolve_target( req
                                           , allow_private_targets
                                           , std::make_shared<dns::Resolver>()
                                           , ctx.get_executor()
                                           , cancel, y));
    });
    ctx.run();
}
