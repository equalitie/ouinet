#define BOOST_TEST_MODULE utility
#include <boost/test/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <async_sleep.h>
#include <task.h>
#include <iostream>
#include <chrono>

using namespace std;
using namespace ouinet;
using namespace chrono;
using namespace chrono_literals;
using Timer = boost::asio::steady_timer;
using Clock = chrono::steady_clock;

auto millis_since(Clock::time_point start) {
    auto end = Clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

BOOST_AUTO_TEST_CASE(test_cancel) {
    using namespace chrono_literals;


    {
        asio::io_context ctx;

        task::spawn_detached(ctx, [&] (asio::yield_context yield) {
                sys::error_code ec;
                Cancel cancel;

                auto start = Clock::now();

                task::spawn_detached(ctx, [&] (asio::yield_context yield) {
                        asio::post(ctx, yield);
                    cancel();
                });

                BOOST_REQUIRE(!cancel);
                async_sleep(1s, cancel, yield[ec]);
                BOOST_REQUIRE(millis_since(start) < 100);
        });

        ctx.run();
    }

    {
        asio::io_context ctx;

        task::spawn_detached(ctx, [&] (asio::yield_context yield) {
                sys::error_code ec;
                Cancel c1;
                Cancel c2 = c1;

                auto start = Clock::now();

                task::spawn_detached(ctx, [c1 = std::move(c1), &ctx]
                                 (asio::yield_context yield) mutable {
                    asio::post(ctx, yield);
                    c1();
                });

                BOOST_REQUIRE(!c1);
                BOOST_REQUIRE(!c2);
                async_sleep(1s, c2, yield[ec]);
                BOOST_REQUIRE(millis_since(start) < 100);
        });

        ctx.run();
    }

    {
        asio::io_context ctx;

        task::spawn_detached(ctx, [&] (asio::yield_context yield) {
                Cancel c;

                {
                    Cancel cc = c;
                }

                c();
        });

        ctx.run();
    }

    {
        Cancel parent;
        unique_ptr<Cancel> child1(new Cancel(parent));
        Cancel child2(std::move(*child1));
        child1 = nullptr;
        parent();
    }
}
