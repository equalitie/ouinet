#define BOOST_TEST_MODULE blocker
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/detached.hpp>
#include <namespaces.h>
#include <util/wait_condition.h>
#include <iostream>
#include <optional>
#include "task.h"

BOOST_AUTO_TEST_SUITE(ouinet_wait_condition)

using namespace std;
using namespace ouinet;
using namespace chrono;
using Timer = boost::asio::steady_timer;
using Clock = chrono::steady_clock;

int millis_since(Clock::time_point start) {
    auto end = Clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

constexpr int waiting_limit = 10;

BOOST_AUTO_TEST_CASE(test_base_functionality) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [&ctx](auto yield) {
        WaitCondition wait_condition(ctx);
        
        optional<milliseconds> actual0, actual1;

        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) {
                auto start = Clock::now();
                Timer timer(ctx);
                timer.expires_after(100ms);
                timer.async_wait(yield);
                actual0 = duration_cast<milliseconds>(Clock::now() - start);
            });
        
        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) {
                auto start = Clock::now();
                Timer timer(ctx);
                timer.expires_after(200ms);
                timer.async_wait(yield);
                actual1 = duration_cast<milliseconds>(Clock::now() - start);
            });
        
        auto start = Clock::now();

        wait_condition.wait(yield); // shall wait 200ms (=max(100ms, 200ms)).
                                    //
        auto elapsed = duration_cast<milliseconds>(Clock::now() - start);
        auto max = std::max(*actual0, *actual1);

        BOOST_TEST(elapsed.count() >= max.count());
        BOOST_TEST(elapsed.count() < max.count() + waiting_limit);
    });

    ctx.run();
}
    
BOOST_AUTO_TEST_CASE(test_release) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [&ctx](auto yield) {
        WaitCondition wait_condition(ctx);
        
        optional<milliseconds> actual0, actual1;

        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) {
                auto start = Clock::now();
                Timer timer(ctx);
                timer.expires_after(100ms);
                timer.async_wait(yield);
                actual0 = duration_cast<milliseconds>(Clock::now() - start);
                // Now we unlock the lock early, so that the wait_condition
                // does not wait for the following sleep operation.
                lock.release();
                timer.expires_after(200ms);
                timer.async_wait(yield);
            });
   
        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) {
                auto start = Clock::now();
                Timer timer(ctx);
                timer.expires_after(200ms);
                timer.async_wait(yield);
                actual1 = duration_cast<milliseconds>(Clock::now() - start);
            });
   
        auto start = Clock::now();
        wait_condition.wait(yield); // shall wait 200ms.

        auto elapsed = duration_cast<milliseconds>(Clock::now() - start);
        auto max = std::max(*actual0, *actual1);

        BOOST_TEST(elapsed.count() >= max.count());
        BOOST_TEST(elapsed.count() < max.count() + waiting_limit);
    });

    ctx.run();
}
    
BOOST_AUTO_TEST_CASE(test_destroy_block_before_wait)
{
    asio::io_context ctx;

    WaitCondition wait_condition(ctx);

    task::spawn_detached(ctx, [&ctx](auto yield) {
        WaitCondition wait_condition(ctx);
        
        {
            auto lock = wait_condition.lock();
        }

        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(ctx);
                timer.expires_after(100ms);
                timer.async_wait(yield);
            });
        
        auto start = Clock::now();
        wait_condition.wait(yield);
        BOOST_TEST(abs(millis_since(start) - 100) < 10);
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
