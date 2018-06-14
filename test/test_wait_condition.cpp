#define BOOST_TEST_MODULE blocker
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <util/wait_condition.h>
#include <iostream>

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

BOOST_AUTO_TEST_CASE(test_base_functionality) {
    asio::io_service ios;

    spawn(ios, [&ios](auto yield) {
        WaitCondition wait_condition(ios);
        
        spawn(ios, [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(100ms);
                timer.async_wait(yield);
            });
        
        spawn(ios, [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(200ms);
                timer.async_wait(yield);
            });
        
        auto start = Clock::now();
        wait_condition.wait(yield); // shall wait 200ms (=max(100ms, 200ms)).
        BOOST_TEST(abs(millis_since(start) - 200) < 10);
    });

    ios.run();
}
    
BOOST_AUTO_TEST_CASE(test_release) {
    asio::io_service ios;

    spawn(ios, [&ios](auto yield) {
        WaitCondition wait_condition(ios);
        
        spawn(ios, [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(100ms);
                timer.async_wait(yield);
                // Now we unlock the lock early, so that the wait_condition
                // does not wait for the following sleep operation.
                lock.release();
                timer.expires_from_now(200ms);
                timer.async_wait(yield);
            });
   
        spawn(ios, [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(200ms);
                timer.async_wait(yield);
            });
   
        auto start = Clock::now();
        wait_condition.wait(yield); // shall wait 200ms.
        BOOST_TEST(abs(millis_since(start) - 200) < 10);
    });

    ios.run();
}
    
BOOST_AUTO_TEST_CASE(test_destroy_block_before_wait)
{
    asio::io_service ios;

    WaitCondition wait_condition(ios);

    spawn(ios, [&ios](auto yield) {
        WaitCondition wait_condition(ios);
        
        {
            auto lock = wait_condition.lock();
        }

        spawn(ios, [&, lock = wait_condition.lock()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(100ms);
                timer.async_wait(yield);
            });
        
        auto start = Clock::now();
        wait_condition.wait(yield);
        BOOST_TEST(abs(millis_since(start) - 100) < 10);
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()

