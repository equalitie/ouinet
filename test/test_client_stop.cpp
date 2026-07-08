#define BOOST_TEST_MODULE test_client_stop
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "namespaces.h"
#include <chrono>
#include "util/test_dir.h"
#include "bittorrent/mock_dht.h"
#include "client.h"

using namespace std;
using namespace ouinet;
using namespace std::chrono_literals;
//using namespace boost::asio::ip;
using bittorrent::MockDht;

constexpr uint16_t wait_for_connections = 12;
constexpr uint16_t wait_to_stop = 5;
constexpr uint16_t MILLISECONDS = 1000;

template<class Config>
static Config make_config(const std::vector<std::string>& args) {
    static constexpr auto c_str = [](const std::string& str) {
        return str.c_str();
    };

    std::vector<const char*> argv;
    std::transform(args.begin(), args.end(), std::back_inserter(argv), c_str);
    return Config(argv.size(), argv.data());
}

void check_exception(std::exception_ptr e) {
    try {
        if (e) {
            std::rethrow_exception(e);
        }
    } catch (const std::exception& e) {
        BOOST_FAIL("Test failed with exception: " << e.what());
    } catch (...) {
        BOOST_FAIL("Test failed with unknown exception");
    }
}

template<class F>
void run(asio::io_context& ctx, F&& async_test) {
    using namespace std::chrono;

    std::optional<steady_clock::time_point> spawn_end;

    asio::spawn(ctx, [&spawn_end, async_test = std::move(async_test)] (asio::yield_context yield) {
            async_test(yield);
            spawn_end = steady_clock::now();
        },
        check_exception);

    ctx.run();

    // Test that after the test ended, the `etx.run()` function exited in a timely manner.
    // If `!spawn_end` then the test threw an exception which already makes the test fail.
    if (spawn_end) {
        auto test_end = steady_clock::now();
        auto elapsed_ms = duration_cast<milliseconds>(test_end - *spawn_end).count();
        // TODO: Keep reducing the allowed timeout
        BOOST_REQUIRE_LT(
            elapsed_ms,
            (wait_for_connections + wait_to_stop) * MILLISECONDS
        );
    }
}


BOOST_AUTO_TEST_CASE(test_client_start_stop) {
    asio::io_context ctx;

    TestDir root;

    auto swarms = std::make_shared<MockDht::Swarms>();

    Client client(ctx, make_config<ClientConfig>({
            "./no_client_exec"s,
            "--log-level=DEBUG"s,
            "--repo"s, root.make_subdir("client").string(),
            "--cache-type=bep5-http"s,
            "--cache-http-public-key=mhwc7k2qui4d3jbrqdbtrahh23auezoiz5sgkg35qmb3j6mvvn2q"s,
            "--udp-mux-port=59846"s,
            // Bind to random ports to avoid clashes
            "--listen-on-tcp=127.0.0.1:0"s,
            "--front-end-ep=127.0.0.1:0"s,
        }),
        util::LogPath("client")
        );

    // Clients are started explicitly
    client.start();

    run(ctx, [&] (asio::yield_context yield) {
        // Wait until incoming uTP connections with `ec="bad method"` come
        asio::steady_timer t(ctx, chrono::seconds(wait_for_connections));
        t.async_wait(yield);

        // Stop should end only a few seconds
        std::cout << "YYY: stopping" << std::endl;
        client.stop();
    });
}
