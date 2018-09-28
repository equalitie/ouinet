#define BOOST_TEST_MODULE blocker
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/io_service.hpp>
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
    asio::io_service ios;

    std::srand(time(0));

    Scheduler scheduler(ios, (std::rand() % 8) + 2);

    unsigned run_count = 0;

    for (unsigned i = 0; i < 20; ++i) {
        spawn(ios, [&ios, &scheduler, &run_count, i](auto yield) {
            ios.post(yield);

            sys::error_code ec;
            auto slot = scheduler.wait_for_slot(yield[ec]);
            BOOST_REQUIRE(!ec);

            ++run_count;
            auto on_exit = defer([&] { --run_count; });

            BOOST_REQUIRE(run_count <= scheduler.max_running_jobs());

            Timer timer(ios);
            timer.expires_from_now(chrono::milliseconds(std::rand() % 500));
            timer.async_wait(yield[ec]);

            BOOST_REQUIRE(!ec);
        });
    }

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_scheduler_destroy_mid_run) {
    asio::io_service ios;

    auto scheduler = make_unique<Scheduler>(ios, 0);

    spawn(ios, [&ios, &scheduler](auto yield) {
        spawn(ios, [&ios, &scheduler](auto yield) {
            ios.post(yield);
            scheduler.reset();
        });

        sys::error_code ec;
        auto slot = scheduler->wait_for_slot(yield[ec]);
        BOOST_REQUIRE_EQUAL(ec, asio::error::operation_aborted);
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()

