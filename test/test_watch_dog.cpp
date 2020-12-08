#define BOOST_TEST_MODULE watch_dog
#include <boost/test/included/unit_test.hpp>

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

BOOST_AUTO_TEST_CASE(test_new_watch_dog) {
    using namespace chrono_literals;

    asio::io_context ctx;
    auto exec = ctx.get_executor();

    auto d = 50ms;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        {
            auto wd = watch_dog(exec, d, [&] { BOOST_REQUIRE(false); });
        }

        {
            sys::error_code ec;

            Cancel cancel;

            auto wd = watch_dog(exec, 1*d, [&] { cancel(); });

            async_sleep(ctx, 2*d, cancel, yield[ec]);

            BOOST_REQUIRE(cancel.call_count() == 1);
        }

        // extend duration
        {
            //  |----+----> first set
            //  |----> sleep
            //       |----+----+----> extend
            //       |----+----> sleep
            //                 |----+----> sleep

            sys::error_code ec;

            Cancel cancel;

            auto wd = watch_dog(exec, 2*d, [&] { cancel(); });

            async_sleep(ctx, 1*d, cancel, yield[ec]);

            BOOST_REQUIRE(wd.is_running());
            wd.expires_after(3*d);

            async_sleep(ctx, 2*d, cancel, yield[ec]);

            BOOST_REQUIRE(wd.is_running());
            BOOST_REQUIRE(cancel.call_count() == 0);

            async_sleep(ctx, 2*d, cancel, yield[ec]);

            BOOST_REQUIRE(!wd.is_running());
            BOOST_REQUIRE(cancel.call_count() == 1);
        }

        // shorten duration
        {
            //  |----+----+----> first set
            //  |----> sleep
            //       |----> shorten
            //       |----+----> sleep
            //            | end

            sys::error_code ec;

            Cancel cancel;

            auto wd = watch_dog(exec, 3*d, [&] { cancel(); });

            async_sleep(ctx, 1*d, cancel, yield[ec]);

            BOOST_REQUIRE(cancel.call_count() == 0);

            wd.expires_after(1*d);

            async_sleep(ctx, 2*d, cancel, yield[ec]);

            BOOST_REQUIRE(cancel.call_count() == 1);
        }
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_old_watch_dog) {
    using namespace chrono_literals;

    asio::io_context ctx;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        {
            WatchDog wd(ctx, 1s, [&] { BOOST_REQUIRE(false); });
        }

        {
            sys::error_code ec;

            Cancel cancel;

            WatchDog wd(ctx, 1s, [&] { cancel(); });

            async_sleep(ctx, 2s, cancel, yield[ec]);

            BOOST_REQUIRE(cancel.call_count());
        }

        // extend duration
        {
            //  |----+----> first set
            //  |----> sleep
            //       |----+----+----> extend
            //       |----+----> sleep
            //                 | end
            sys::error_code ec;

            Cancel cancel;

            WatchDog wd(ctx, 2s, [&] { cancel(); });

            async_sleep(ctx, 1s, cancel, yield[ec]);

            wd.expires_after(3s);

            async_sleep(ctx, 2s, cancel, yield[ec]);

            BOOST_REQUIRE(cancel.call_count() == 0);
        }
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
