#define BOOST_TEST_MODULE blocker
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <util/scheduler.h>
#include <defer.h>
#include <iostream>

BOOST_AUTO_TEST_SUITE(ouinet_scheduler)

using namespace std;
using namespace ouinet;
using namespace chrono;
using Timer = boost::asio::steady_timer;
using Clock = chrono::steady_clock;

int millis_since(Clock::time_point start) {
    auto end = Clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

BOOST_AUTO_TEST_CASE(test_scheduler) {
    asio::io_context ctx;

    std::srand(time(0));

    Scheduler scheduler(ctx, (std::rand() % 8) + 2);

    unsigned run_count = 0;

    for (unsigned i = 0; i < 20; ++i) {
        spawn(ctx, [&ctx, &scheduler, &run_count, i](auto yield) {
            ctx.post(yield);

            sys::error_code ec;
            auto slot = scheduler.wait_for_slot(yield[ec]);
            BOOST_REQUIRE(!ec);

            ++run_count;
            auto on_exit = defer([&] { --run_count; });

            BOOST_REQUIRE(run_count <= scheduler.max_running_jobs());

            Timer timer(ctx);
            timer.expires_from_now(chrono::milliseconds(std::rand() % 500));
            timer.async_wait(yield[ec]);

            BOOST_REQUIRE(!ec);
        });
    }

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_scheduler_cancel) {
    asio::io_context ctx;

    Scheduler scheduler(ctx, 0);

    spawn(ctx, [&ctx, &scheduler](auto yield) {
        Cancel cancel;
        spawn(ctx, [&ctx, &scheduler, &cancel](auto yield) {
            ctx.post(yield);
            cancel();
        });
        sys::error_code ec;
        auto slot = scheduler.wait_for_slot(cancel, yield[ec]);
        BOOST_REQUIRE_EQUAL(ec, asio::error::operation_aborted);
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_scheduler_destroy_mid_run) {
    asio::io_context ctx;

    auto scheduler = make_unique<Scheduler>(ctx, 0);

    spawn(ctx, [&ctx, &scheduler](auto yield) {
        spawn(ctx, [&ctx, &scheduler](auto yield) {
            ctx.post(yield);
            scheduler.reset();
        });

        sys::error_code ec;
        auto slot = scheduler->wait_for_slot(yield[ec]);
        BOOST_REQUIRE_EQUAL(ec, asio::error::operation_aborted);
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

