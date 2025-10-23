#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include "task.h"
#include "cxx/dns.h"

using namespace boost;
using namespace ouinet;

BOOST_AUTO_TEST_SUITE(ouinet_dns)

BOOST_AUTO_TEST_CASE(sanity) {
    asio::io_context ctx;
    dns::Resolver resolver;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto result = resolver.resolve("ceno.app", yield);

        BOOST_REQUIRE(result.has_value());

        auto ips = result.value();

        for (auto ip : ips) {
            std::cout << ip << std::endl;
        }
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
