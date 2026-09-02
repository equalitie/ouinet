#define BOOST_TEST_MODULE wait_watch

#include <boost/test/unit_test.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>

#include "util/watch.h"
#include "namespaces.h"
#include "async_sleep.h"
#include "task.h"

using namespace ouinet;
using namespace std::chrono_literals;

template<class F> void run(F f) {
    asio::io_context ctx;
    auto exec = ctx.get_executor();

    task::spawn_detached(ctx.get_executor(), [f = std::move(f)](asio::yield_context yield) {
        try {
            f(Async(yield, {}, {}));
        }
        catch (const std::exception& e) {
            BOOST_FAIL("Exception " << e.what());
        }
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(example) {
    run([] (Async yield) {
        auto [tx, rx] = Watch<uint32_t>::create(yield.get_executor(), 0);

        static const uint32_t max_value = 5;

        yield.spawn([tx = std::move(tx)] (Async yield) mutable {
            for (uint32_t value = 1; value <= 5; value++) {
                async_sleep(100ms, yield);
                tx.send(value);
            }
        });

        std::optional<uint32_t> last_value;

        while (rx.await_change(yield)) {
            if (last_value) {
                BOOST_REQUIRE_GT(rx.value(), *last_value);
            }
            last_value = rx.value();
        }

        BOOST_REQUIRE_EQUAL(rx.value(), max_value);
    });
}
