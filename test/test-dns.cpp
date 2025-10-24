#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include "cxx/dns.h"

using namespace boost;
using namespace ouinet;

BOOST_AUTO_TEST_SUITE(ouinet_dns)

BOOST_AUTO_TEST_CASE(sanity) {
    asio::io_context ctx;

    asio::spawn(ctx,
        [] (asio::yield_context yield) {
            dns::Resolver resolver;

            auto ips = resolver.resolve("ceno.app", yield);

            for (auto ip : ips) {
                std::cout << ip << std::endl;
            }
        },
        [] (std::exception_ptr ep) {
            if (ep) std::rethrow_exception(ep);
        }
    );

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
