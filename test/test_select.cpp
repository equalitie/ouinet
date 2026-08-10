#define BOOST_TEST_MODULE select
#include <boost/test/unit_test.hpp>

#include <chrono>

#include "async_sleep.h"
#include "defer.h"
#include "util/select.h"
#include "util/wait_condition.h"

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
BOOST_AUTO_TEST_CASE(delayed_branch_cancel) {
    async_test([] (Async yield) {
        std::array<bool, 2> completed = { false, false };

        select(
            yield,
            [&] (Async yield) {
                asio::post(yield);
                completed[0] = true;
            },
            [&] (Async yield) {
                {
                    auto non_cancellable = yield.suppress_cancel();
                    for (int i = 0; i < 100; ++i) {
                        asio::post(non_cancellable);
                    }
                }

                completed[1] = true;
            }
        );

        BOOST_REQUIRE(completed[0]);
        BOOST_REQUIRE(completed[1]);
    });
}

// If the select itself gets cancelled, it must wait for all branches to complete but then propagate
// the cancellation (by throwing the `Async::Cancelled` exception) to the caller.
BOOST_AUTO_TEST_SUITE(cancel);

    // Cancellation happens while the select is waiting for the first branch to complete.
    BOOST_AUTO_TEST_CASE(waiting_for_branch) {
        async_test([] (Async yield) {
            WaitCondition all_completed_wc(yield.get_executor());

            bool branch_0_completed = false;
            bool branch_0_cancelled = false;

            bool branch_1_completed = false;
            bool branch_1_cancelled = false;

            bool all_completed = false;
            bool all_cancelled = false;

            Cancel cancel;
            yield.spawn(cancel, [&, lock = all_completed_wc.lock()] (Async yield) {
                try {
                    select(
                        yield,
                        [&] (Async yield) {
                            try {
                                async_sleep(std::chrono::seconds(5), yield);
                                branch_0_completed = true;
                            } catch (const Async::Cancelled&) {
                                branch_0_cancelled = true;
                            }
                        },
                        [&] (Async yield) {
                            try {
                                async_sleep(std::chrono::seconds(6), yield);
                            } catch (const Async::Cancelled&) {
                                branch_1_cancelled = true;
                            }

                            // non-cancellable cleanup work
                            {
                                auto non_cancellable = yield.suppress_cancel();
                                for (int i = 0; i < 100; ++i) {
                                    asio::post(non_cancellable);
                                }
                            }

                            branch_1_completed = true;
                        }
                    );

                    all_completed = true;
                } catch (const Async::Cancelled&) {
                    all_cancelled = true;
                }
            });

            asio::post(yield);
            cancel();

            all_completed_wc.wait(yield).value();

            BOOST_REQUIRE(!branch_0_completed);
            BOOST_REQUIRE(branch_0_cancelled);

            BOOST_REQUIRE(branch_1_completed);
            BOOST_REQUIRE(branch_1_cancelled);

            BOOST_REQUIRE(!all_completed);
            BOOST_REQUIRE(all_cancelled);
        });
    }

    // Cancellation happens while the select is waiting for the remaining branches to complete after
    // the first branch already completed.
    BOOST_AUTO_TEST_CASE(waiting_for_cleanup) {
        async_test([] (Async yield) {
            WaitCondition branch_0_completed_wc(yield.get_executor());

            WaitCondition branch_1_started_wc(yield.get_executor());
            auto branch_1_started_lock = branch_1_started_wc.lock();

            WaitCondition all_completed_wc(yield.get_executor());

            bool branch_0_completed = false;
            bool branch_0_cancelled = false;

            bool branch_1_completed = false;
            bool branch_1_cancelled = false;

            bool all_completed = false;
            bool all_cancelled = false;

            Cancel cancel;
            yield.spawn(cancel, [&, lock = all_completed_wc.lock()] (Async yield) {
                try {
                    select(
                        yield,
                        [&, lock = branch_0_completed_wc.lock()] (Async yield) {
                            try {
                                asio::post(yield);
                                branch_0_completed = true;
                            } catch (const Async::Cancelled&) {
                                branch_0_cancelled = true;
                            }
                        },
                        [&] (Async yield) {
                            branch_1_started_wc.wait(yield.suppress_cancel()).value();
                            branch_1_completed = true;
                            branch_1_cancelled = yield.is_cancelled();
                        }
                    );
                    all_completed = true;
                } catch (const Async::Cancelled&) {
                    all_cancelled = true;
                }
            });

            // Wait until branch 0 completes
            branch_0_completed_wc.wait(yield).value();
            BOOST_REQUIRE(branch_0_completed);
            BOOST_REQUIRE(!branch_0_cancelled);

            // The select itself should still keep running because it's waiting for branch 1 to complete
            BOOST_REQUIRE(!all_completed);
            BOOST_REQUIRE(!all_cancelled);

            // Cancel the select, wait for 1 tick and then unblock branch 1
            cancel();
            asio::post(yield);
            branch_1_started_lock.release();

            // Wait until the select completes
            all_completed_wc.wait(yield).value();

            BOOST_REQUIRE(branch_1_completed);
            BOOST_REQUIRE(branch_1_cancelled);

            BOOST_REQUIRE(!all_completed);
            BOOST_REQUIRE(all_cancelled);
        });
    }

BOOST_AUTO_TEST_SUITE_END(); // cancel
