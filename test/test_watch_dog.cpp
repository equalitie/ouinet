#define BOOST_TEST_MODULE watch_dog
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <util/watch_dog.h>
#include <async_sleep.h>
#include <iostream>

BOOST_AUTO_TEST_SUITE(ouinet_watch_dog)

using namespace std;
using namespace ouinet;
using namespace chrono;
using Timer = boost::asio::steady_timer;
using Clock = chrono::steady_clock;

int millis_since(Clock::time_point start) {
    auto end = Clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

BOOST_AUTO_TEST_CASE(test_watch_dog) {
    using namespace chrono_literals;

    asio::io_service ios;

    asio::spawn(ios, [&] (asio::yield_context yield) {
        {
            WatchDog wd(ios, 1s, [&] { BOOST_REQUIRE(false); });
        }

        {
            sys::error_code ec;

            Cancel cancel;

            auto start = Clock::now();
            WatchDog wd(ios, 1s, [&] { cancel(); });

            async_sleep(ios, 2s, cancel, yield[ec]);

            BOOST_REQUIRE(cancel.call_count());
        }
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
