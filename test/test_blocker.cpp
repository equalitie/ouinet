#define BOOST_TEST_MODULE blocker
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <blocker.h>
#include <iostream>

BOOST_AUTO_TEST_SUITE(ouinet_blocker)

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
        Blocker blocker(ios);
        
        spawn(ios, [&, b = blocker.make_block()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(100ms);
                timer.async_wait(yield);
            });
        
        spawn(ios, [&, b = blocker.make_block()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(200ms);
                timer.async_wait(yield);
            });
        
        auto start = Clock::now();
        blocker.wait(yield); // shall wait 200ms (=max(100ms, 200ms)).
        BOOST_TEST(abs(millis_since(start) - 200) < 10);
    });

    ios.run();
}
    
BOOST_AUTO_TEST_CASE(test_release) {
    asio::io_service ios;

    spawn(ios, [&ios](auto yield) {
        Blocker blocker(ios);
        
        spawn(ios, [&, b = blocker.make_block()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(100ms);
                timer.async_wait(yield);
                // Now we instruct the 'blocker' to no longer wait
                // for the remaining blocks to get destroyed.
                b.release();
            });
   
        spawn(ios, [&, b = blocker.make_block()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(200ms);
                timer.async_wait(yield);
            });
   
        auto start = Clock::now();
        blocker.wait(yield); // shall wait 100ms.
        BOOST_TEST(abs(millis_since(start) - 100) < 10);
    });

    ios.run();
}
    
BOOST_AUTO_TEST_CASE(test_destroy_block_before_wait)
{
    asio::io_service ios;

    Blocker blocker(ios);

    spawn(ios, [&ios](auto yield) {
        Blocker blocker(ios);
        
        {
            auto block = blocker.make_block();
        }

        spawn(ios, [&, b = blocker.make_block()](auto yield) {
                Timer timer(ios);
                timer.expires_from_now(100ms);
                timer.async_wait(yield);
            });
        
        auto start = Clock::now();
        blocker.wait(yield);
        BOOST_TEST(abs(millis_since(start) - 100) < 10);
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()

