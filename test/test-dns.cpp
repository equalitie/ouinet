#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include "cxx/dns.h"
#include "task.h"

using namespace boost;
using namespace boost::asio;
using namespace boost::system;
using namespace ouinet;

BOOST_AUTO_TEST_SUITE(ouinet_dns)

BOOST_AUTO_TEST_CASE(valid_name) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [] (asio::yield_context yield) {
        dns::Resolver resolver;

        auto expected = ip::address_v4({23, 215, 0, 136});
        auto actual = resolver.resolve("example.com", yield);

        BOOST_REQUIRE(std::find(actual.begin(), actual.end(), expected) != actual.end());
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(invalid_name) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [] (asio::yield_context yield) {
        dns::Resolver resolver;
        error_code ec;

        auto actual = resolver.resolve("example.invalid", yield[ec]);

        BOOST_REQUIRE(actual.empty());
        BOOST_REQUIRE_EQUAL(ec, error_code(dns::Error::NotFound));
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
