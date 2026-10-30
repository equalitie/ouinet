#include "util/wait_condition.h"
#define BOOST_TEST_MODULE test_injector
#include <boost/test/unit_test.hpp>

#include "util/test_dir.h"
#include "async_sleep.h"
#include "injector.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace std::chrono_literals;
using namespace boost::asio::ip;

template <class Config>
static Config make_config(const std::vector<std::string> &args) {
  static constexpr auto c_str = [](const std::string &str) {
    return str.c_str();
  };

  std::vector<const char *> argv;
  std::transform(args.begin(), args.end(), std::back_inserter(argv), c_str);
  return Config(argv.size(), argv.data());
}

void check_exception(std::exception_ptr e) {
  try {
    if (e) {
      std::rethrow_exception(e);
    }
  } catch (const std::exception &e) {
    BOOST_FAIL("Test failed with exception: " << e.what());
  } catch (...) {
    BOOST_FAIL("Test failed with unknown exception");
  }
}

template <class F>
  requires std::invocable<F, Async>
void run(asio::io_context &ctx, F &&async_test) {
  using namespace std::chrono;

  std::optional<steady_clock::time_point> spawn_end;

  asio::spawn(
    ctx,
    [&spawn_end, async_test = std::move(async_test)](
  asio::yield_context yield) mutable {
      async_test(Async(yield));
      spawn_end = steady_clock::now();
    },
    check_exception
  );

  ctx.run();
}

void bep5_spam(const asio::yield_context &yield,
               const udp::endpoint &target_ep) {
  std::string msg = "d1:ad2:id20:abcdefghij0123456789e1:q4:ping1:t2:aa1:y1:qe";
  udp::socket udp_sock{yield.get_executor()};
  udp_sock.open(udp::v4());
  Cancel cancel;
  for (auto i = 0; i < 100000; i++) {
    std::ignore = udp_sock.send_to(
      asio::buffer(msg.c_str(), msg.size()),
      target_ep
    );
    async_sleep(10us, cancel, yield);
  }
  udp_sock.close();
}

BOOST_AUTO_TEST_CASE(
  test_injector_accept_dht_queries
) {
  asio::io_context ctx;
  run(ctx, [&ctx](Async yield) {
    auto exec = ctx.get_executor();
    const TestDir root;
    udp::endpoint injector_ep{
      address_v4::loopback(),
      7085
    };

    Injector injector(
      make_config<InjectorConfig>({
        "./no_injector_exec"s,
        // This test produces a huge number of `LOG_WARN` lines from inside
        // `mainline_dht.cpp`, if they are printed on the CI the log output
        // exceeds the maximum limit and we won't see anything else.
        "--log-level=ERROR",
        "--repo"s, root.make_subdir("injector").string(),
        "--listen-on-utp=0.0.0.0:"s + to_string(injector_ep.port()),
        // TODO: bind to random port
      }),
      ctx
    );

    WaitCondition spam_sent{exec};
    task::spawn_detached(exec, [&, lock=spam_sent.lock() ](auto y) {
      bep5_spam(y, injector_ep);
    });
    std::ignore = spam_sent.wait(yield);
  });
}
