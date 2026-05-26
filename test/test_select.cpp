#define BOOST_TEST_MODULE select
#include <boost/test/unit_test.hpp>

#include <chrono>

#include <async_sleep.h>
#include <util/select.h>
#include <util/wait_condition.h>

#include "util/async_test.h"

using namespace ouinet;

BOOST_AUTO_TEST_CASE(select_sanity_check) {
    auto subcase = [](int branch) {
        async_test([=](Async yield) {
            std::array wcs {
                WaitCondition(yield.get_executor()),
                WaitCondition(yield.get_executor())
            };

            std::array locks = { wcs[0].lock(), wcs[1].lock() };

            asio::post(yield.get_executor(), [lock = std::move(locks[branch])]() {});

            auto result = select(
                yield,
                [&](Async yield) {
                    wcs[0].wait(yield);
                    return 0;
                },
                [&](Async yield) {
                    wcs[1].wait(yield);
                    return 1;
                }
            );

            BOOST_REQUIRE_EQUAL(result, branch);
        });
    };

    subcase(0);
    subcase(1);
}

BOOST_AUTO_TEST_CASE(timeout_sanity_check) {
    async_test([](Async yield) {
       auto result = timeout(std::chrono::milliseconds(100), [](auto yield) {
           async_sleep(std::chrono::milliseconds(200), yield);
           return 1;
       }, yield);

       BOOST_REQUIRE(!result.has_value());
    });

    async_test([](Async yield) {
       auto result = timeout(std::chrono::milliseconds(200), [](auto yield) {
           async_sleep(std::chrono::milliseconds(100), yield);
           return 1;
       }, yield);

       BOOST_REQUIRE_EQUAL(result.value(), 1);
    });
}
