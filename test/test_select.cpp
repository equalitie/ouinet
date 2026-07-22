#include "defer.h"
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

BOOST_AUTO_TEST_CASE(select_void) {
    async_test([](Async yield) {
        int result = 0;

        select(
            yield,
            [&](auto yield) {
                result = 1;
            },
            [&](auto yield) {
                result = 2;
            }
        );

        BOOST_REQUIRE_GT(result, 0);
    });
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

// Some async function might still do some processing (even async) after being cancelled instead of
// returning immediately. This test ensures that `select` waits for them to complete before itself
// returning.
BOOST_AUTO_TEST_CASE(delayed_cancel) {
    async_test([] (Async yield) {
        std::array<bool, 2> completed = { false, false };

        select(
            yield,
            [&] (Async yield) {
                auto cleanup = defer([&] {
                    completed[0] = true;
                });

                asio::post(yield);
            },
            [&] (Async yield) {
                auto cleanup = defer([&] {
                    completed[1] = true;
                });

                asio::post(yield);
                asio::post(yield);
                asio::post(yield);
            }
        );

        BOOST_REQUIRE(completed[0]);
        BOOST_REQUIRE(completed[1]);
    });
}
