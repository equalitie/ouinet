#define BOOST_TEST_MODULE select
#include <boost/test/unit_test.hpp>

#include <util/select.h>
#include <util/wait_condition.h>

#include "util/async_test.h"

using namespace ouinet;

BOOST_AUTO_TEST_CASE(sanity_check) {
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
