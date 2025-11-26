#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/spawn.hpp>
#include "cxx/dns.h"
#include "task.h"

using namespace boost::asio;
using namespace boost::system;
using namespace ouinet;

BOOST_AUTO_TEST_SUITE(ouinet_dns)

BOOST_AUTO_TEST_CASE(valid_name) {
    io_context ctx;

    spawn(ctx, [](yield_context yield) {
        dns::Resolver resolver;

        auto expected = ip::address_v4({23, 215, 0, 136});
        auto actual = resolver.resolve("example.com", yield);

        BOOST_REQUIRE(std::find(actual.begin(), actual.end(), expected) != actual.end());
    }, [](auto e) {
        if (e) std::rethrow_exception(e);
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(invalid_name) {
    io_context ctx;

    spawn(ctx, [](yield_context yield) {
        dns::Resolver resolver;

        BOOST_REQUIRE_EXCEPTION(
            resolver.resolve("example.invalid", yield),
            system_error,
            [](auto e) {
                return e.code() == dns::Error::NotFound;
            }
        );
    }, [](auto e) {
        if (e) std::rethrow_exception(e);
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(cancellation_per_object) {
    io_context ctx;

    dns::Resolver resolver;

    spawn(ctx, [&](yield_context yield) {
        BOOST_REQUIRE_EXCEPTION(
            resolver.resolve("example.com", yield),
            system_error,
            [](auto e) {
                return e.code() == error::operation_aborted;
            }
        );
    }, [](auto e) {
        if (e) std::rethrow_exception(e);
    });

    post(ctx, [&]() {
        resolver.close();
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(cancellation_per_operation) {
    io_context ctx;

    auto cancellation_signal = asio::cancellation_signal();

    spawn(
        ctx,
        [&](yield_context yield) {
            dns::Resolver resolver;

            BOOST_REQUIRE_EXCEPTION(
                resolver.resolve("example.com", yield),
                system_error,
                [](auto e) {
                    return e.code() == error::operation_aborted;
                }
            );
        },
        bind_cancellation_slot(cancellation_signal.slot(), [](auto e) {
            if (e) std::rethrow_exception(e);
        })
    );

    post(ctx, [&]() {
        cancellation_signal.emit(cancellation_type::all);
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
