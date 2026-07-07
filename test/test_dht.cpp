#include "asio_utp/udp_multiplexer.hpp"
#define BOOST_TEST_MODULE test_dht
#include <boost/test/unit_test.hpp>
#include <boost/asio.hpp>

#include <iomanip>
#include <chrono>
#include <constants.h>
#include <util/compat.h>
#include <util/debug.h>
#include <util/hash.h>

#define private public
#include <bittorrent/dht_node.h>
#undef private
#include <bittorrent/dht_storage.h>
#include <bittorrent/mainline_dht.h>
#include <bittorrent/node_id.h>
#include <bittorrent/udp_multiplexer.h>

#include "util/async_test.h"
#include "util/dht.h"
#include "util/unwrap.h"

using namespace std;
using namespace chrono;
using namespace ouinet;
using namespace ouinet::bittorrent;

using Clock = chrono::steady_clock;

// This should be in line with `bootstrap::bootstraps`, defined in `src/bittorrent/dht.cpp:1711`
vector<bootstrap::Address> bootstraps {
        "dht.libtorrent.org:25401"
        , "dht.transmissionbt.com:6881"
        // Alternative bootstrap servers from the Ouinet project.
        , "router.bt.ouinet.work"
        // Part of previous name (in case of DNS failure).
        , asio::ip::make_address("168.222.245.126")
        , "routerx.bt.ouinet.work:5060"  // squat popular UDP high port (SIP)
};

void init_without_bootstrapping(asio::any_io_executor exec, DhtNode& dht_node) {
    task::spawn_detached(exec, [&](auto yield) {
        dht_node._tracker = make_unique<Tracker>(exec);
        dht_node._data_store = make_unique<DataStore>(exec);

        dht_node._node_id = NodeID::zero();
        dht_node._next_transaction_id = 1;
    });

    task::spawn_detached(exec, [&](auto yield) {
        dht_node.receive_loop(Async(yield));
    });
}

void do_bootstrap(asio::any_io_executor exec, DhtNode& dht_node) {
    task::spawn_detached(exec, [&](auto yield) {
        size_t success{0};

        cout << "server\t"
                  << "my_ep\t"
                  << "node_ep\t"
                  << "ec_value\t"
                  << "ec_message\t"
                  << "elapsed_seconds"
                  << endl;

        for (const auto &bs : bootstraps) {
            sys::error_code ec;
            Clock::time_point start;
            Clock::time_point now;

            start = Clock::now();
            auto r = compat([&](Async yield) {
                return dht_node.bootstrap_single(bs, yield);
            })(dht_node._cancel, yield[ec]);
            now = Clock::now();
            auto elapsed = duration_cast<seconds>(now - start).count();

            cout << bs << "\t"
                      << r.my_ep << "\t"
                      << r.node_ep << "\t"
                      << ec.value()  << "\t"
                      << ec.message() << "\t"
                      << elapsed << endl;

            if (!ec) success++;
        }


        auto success_rate = static_cast<double>(success) / static_cast<double>(bootstraps.size()) * 100.;
        cout << "Success rate: "
                  << fixed << setprecision(0)
                  << success_rate << "% "
                  << "(" << success << " of " << bootstraps.size() << ")" << endl;
        dht_node.stop();

        BOOST_TEST_CHECK(success_rate >= 40);
    });
}

BOOST_AUTO_TEST_CASE(test_bootstrap)
{
    asio::io_context ctx;
    auto exec = ctx.get_executor();

    auto metrics_client = metrics::Client();
    auto metrics_dht = metrics_client.mainline_dht();
    auto dns_resolver = std::make_shared<dns::Resolver>();
    uint32_t rx_limit = udp_mux_rx_limit_client;

    asio_utp::udp_multiplexer socket(exec);
    sys::error_code ec;
    socket.bind({asio::ip::address_v4::any(), 0}, ec);

    DhtNode dht_node(
        std::move(socket),
        metrics_dht.dht_node_ipv4(),
        dns_resolver,
        rx_limit,
        {},
        {},
        {}
    );

    init_without_bootstrapping(exec, dht_node);
    do_bootstrap(exec, dht_node);
    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_local)
{
    get_logger().set_threshold(DEBUG);

    async_test([](Async yield) {
        auto start = steady_clock::now();
        auto nodes = spawn_dht_nodes(8, yield);
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start);

        cout << nodes.size() << " nodes bootstrapped in " << elapsed.count() << "ms." << endl;

        NodeID infohash = util::sha1_digest("hello world");

        {
            auto peers = unwrap(nodes[0]->tracker_announce(infohash, std::nullopt, yield));
            BOOST_REQUIRE(peers.empty());
        }

        {
            auto actual = unwrap(nodes[1]->tracker_get_peers(infohash, yield));
            auto expected = nodes[0]->local_endpoints();

            BOOST_REQUIRE_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
        }
    });
}
