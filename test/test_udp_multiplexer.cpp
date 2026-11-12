#define BOOST_TEST_MODULE test_udp_multiplexer
#include "asio_utp.hpp"
#include "util/async.h"
#include <bittorrent/udp_multiplexer.h>
#include <boost/asio.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <utility>

namespace data = boost::unit_test::data;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace boost::asio::ip;
using namespace std::chrono;

static void check_exception(const std::exception_ptr &e) {
  try {
    if (e) {
      std::rethrow_exception(e);
    }
  } catch (const std::exception &exception) {
    BOOST_FAIL("Test failed with exception: " << exception.what());
  } catch (...) {
    BOOST_FAIL("Test failed with unknown exception");
  }
}

template <class F>
  requires std::invocable<F, Async>
static void run(asio::io_context &ctx, F &&async_test) {
  asio::spawn(
      ctx,
      [async_test =
           std::forward<F>(async_test)](asio::yield_context yield) mutable {
        async_test(Async(std::move(yield)));
      },
      check_exception);

  ctx.run();
}

BOOST_DATA_TEST_CASE(test_rate_limiter,
                     data::make({0u, 100u, 200u, 500u, 1000u}) ^
                         data::make({1000, 1000, 1000, 1000, 1000}) ^
                         data::make({5, 10000, 5000, 2000, 5}),
                     limit, current, expected_delay_ms) {
  asio::io_context ctx;
  run(ctx, [exec = ctx.get_executor(), limit, expected_delay_ms,
            current](const Async &yield) {
    Cancel cancel;
    RateCounter counter{};
    counter.update(current);
    auto limiter = RateLimiter{exec, counter, limit, cancel};

    auto start_time = steady_clock::now();
    limiter.maintain_max_rate_bytes_per_sec(yield.asio_yield());
    auto end_time = steady_clock::now();

    auto elapsed_ms =
        duration_cast<milliseconds>(end_time - start_time).count();
    BOOST_TEST_MESSAGE("Elapsed time: " << elapsed_ms << " ms");
    BOOST_REQUIRE_LT(elapsed_ms, expected_delay_ms);
  });
}

BOOST_DATA_TEST_CASE(test_rate_limiter_cancellation,
                     data::make({100u, 200u, 500u}), limit) {
  asio::io_context ctx;
  Cancel cancel;
  auto cancel_delay = 1000;

  asio::spawn(
      ctx,
      [&cancel, &cancel_delay,
       exec = ctx.get_executor()](const asio::yield_context &y) mutable {
        const auto delay = milliseconds(static_cast<int>(cancel_delay));
        asio::steady_timer timer{exec};
        timer.expires_after(delay);
        timer.async_wait(y);
        cancel();
      },
      check_exception);

  run(ctx, [exec = ctx.get_executor(), limit, &cancel](const Async &yield) {
    constexpr float current = 2000;
    constexpr auto expected_delay_ms = 1500;
    RateCounter counter{};
    counter.update(current);
    auto limiter = RateLimiter{exec, counter, limit, cancel};

    const auto start_time = steady_clock::now();
    limiter.maintain_max_rate_bytes_per_sec(yield.asio_yield());
    const auto end_time = steady_clock::now();

    const auto elapsed_ms =
        duration_cast<milliseconds>(end_time - start_time).count();
    BOOST_REQUIRE(cancel);
    BOOST_REQUIRE_LT(elapsed_ms, expected_delay_ms);
  });
}
