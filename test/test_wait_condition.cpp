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
#include <chrono>
#include "task.h"

using namespace std;
using namespace ouinet;
using namespace chrono;
using Timer = boost::asio::steady_timer;
using Clock = chrono::steady_clock;

int millis_since(Clock::time_point start) {
    auto end = Clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

// On Windows CI this is sometimes up to 10 milliseconds
constexpr milliseconds wait_limit = 10ms;

BOOST_AUTO_TEST_CASE(one) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [](auto yield) {
        WaitCondition wait_condition(yield.get_executor());
        
        milliseconds wait_duration = 100ms;

        task::spawn_detached(yield.get_executor(), [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(yield.get_executor());
                timer.expires_after(wait_duration);
                timer.async_wait(yield);
            });
        
        auto start = Clock::now();

        wait_condition.wait(yield);

        auto elapsed = duration_cast<milliseconds>(Clock::now() - start);

        BOOST_TEST(elapsed >= wait_duration);
        BOOST_TEST(elapsed - wait_duration <= wait_limit);
    });

    ctx.run();
}
    
BOOST_AUTO_TEST_CASE(one_with_callback) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [](auto yield) {
        auto wait_condition = std::make_shared<WaitCondition>(yield.get_executor());
        
        milliseconds wait_duration = 100ms;

        auto start = Clock::now();

        auto timer = std::make_shared<Timer>(yield.get_executor());
        timer->expires_after(wait_duration);
        timer->async_wait([timer, lock = wait_condition->lock()] (sys::error_code) {});

        wait_condition->wait([wait_condition, wait_duration, start] (sys::error_code ec) {
            BOOST_REQUIRE(!ec);

            auto elapsed = duration_cast<milliseconds>(Clock::now() - start);

            BOOST_TEST(elapsed >= wait_duration);
            BOOST_TEST(elapsed - wait_duration <= wait_limit);
        });
    });

    ctx.run();
}
    
BOOST_AUTO_TEST_CASE(two) {
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

        auto elapsed = duration_cast<milliseconds>(Clock::now() - start);
        auto max = std::max(*actual0, *actual1);

        BOOST_TEST(elapsed >= max);
        BOOST_TEST(elapsed < max + wait_limit);
    });

    ctx.run();
}
    
BOOST_AUTO_TEST_CASE(test_release) {
    asio::io_context ctx;

    task::spawn_detached(ctx, [&ctx](auto yield) {
        WaitCondition wait_condition(ctx);
        
        optional<milliseconds> actual0, actual1;

        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) mutable {
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
   
        task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) mutable {
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

        BOOST_TEST(elapsed >= max);
        BOOST_TEST(elapsed < max + wait_limit);
    });

    ctx.run();
}
    
BOOST_AUTO_TEST_CASE(two_consumers) {
    asio::io_context ctx;

    WaitCondition wait_condition(ctx);
    
    optional<milliseconds> actual0;

    milliseconds sleep_duration = 200ms;

    task::spawn_detached(ctx, [&, lock = wait_condition.lock()](auto yield) {
            auto start = Clock::now();

            Timer timer(ctx);
            timer.expires_after(sleep_duration);
            timer.async_wait(yield);

            actual0 = duration_cast<milliseconds>(Clock::now() - start);
        });
   
    milliseconds d0, d1;

    task::spawn_detached(ctx, [&](auto yield) {
            auto start = Clock::now();
            wait_condition.wait(yield);
            d0 = duration_cast<milliseconds>(Clock::now() - start);
        });
   
    task::spawn_detached(ctx, [&](auto yield) {
            auto start = Clock::now();
            wait_condition.wait(yield);
            d1 = duration_cast<milliseconds>(Clock::now() - start);
        });
   
    ctx.run();

    BOOST_TEST(std::chrono::abs(sleep_duration - d0) < wait_limit);
    BOOST_TEST(std::chrono::abs(sleep_duration - d1) < wait_limit);
}
    
BOOST_AUTO_TEST_CASE(destroy_lock_before_wait)
{
    asio::io_context ctx;

    WaitCondition wait_condition(ctx);

    task::spawn_detached(ctx, [&](auto yield) {
        WaitCondition wait_condition(ctx);
        
        {
            auto lock = wait_condition.lock();
        }

        auto start = Clock::now();

        wait_condition.wait(yield);

        auto elapsed = duration_cast<milliseconds>(Clock::now() - start);

        BOOST_TEST(elapsed < wait_limit);
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(destroy_lock_before_wait_then_relock)
{
    asio::io_context ctx;

    WaitCondition wait_condition(ctx);

    milliseconds sleep_for = 100ms;

    task::spawn_detached(ctx, [&](auto yield) {
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

        auto elapsed = duration_cast<milliseconds>(Clock::now() - start);

        BOOST_TEST(std::chrono::abs(elapsed - sleep_for) < wait_limit);
    });

    ctx.run();
}
