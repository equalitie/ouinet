#define BOOST_TEST_MODULE dht
#include <boost/test/included/unit_test.hpp>
#include <boost/asio.hpp>

#include <chrono>
#include <util/hash.h>

#define private public
#include <bittorrent/dht.h>
#include <bittorrent/node_id.h>
#include <bittorrent/udp_multiplexer.h>

BOOST_AUTO_TEST_SUITE(dht)

using namespace std;
using namespace chrono;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;

using Clock = chrono::steady_clock;

// This should be in line with `bootstrap::bootstraps`, defined in `src/bittorrent/dht.cpp:1711`
vector<bootstrap::Address> bootstraps {
        "dht.libtorrent.org:25401"
        , "dht.transmissionbt.com:6881"
        // Alternative bootstrap servers from the Ouinet project.
        , "router.bt.ouinet.work"
        // Part of previous name (in case of DNS failure).
        , asio::ip::make_address("74.3.163.127")
        , "routerx.bt.ouinet.work:5060"  // squat popular UDP high port (SIP)
};

void init_without_bootstrapping(asio::io_context& ctx, DhtNode& dht_node) {
    task::spawn_detached(ctx, [&](auto yield) {
        sys::error_code ec;
        auto local_ep = udp::endpoint{asio::ip::make_address("0.0.0.0"), 0};
        auto m = asio_utp::udp_multiplexer(ctx);
        m.bind(local_ep, ec);

        dht_node._multiplexer = make_unique<UdpMultiplexer>(move(m));
        dht_node._tracker = make_unique<Tracker>(ctx.get_executor());
        dht_node._data_store = make_unique<DataStore>(ctx.get_executor());

        dht_node._node_id = NodeID::zero();
        dht_node._next_transaction_id = 1;
    });

    task::spawn_detached(ctx, [&](auto yield) {
        dht_node.receive_loop(yield);
    });
}

void bootstrap(asio::io_context& ctx, DhtNode& dht_node) {
    task::spawn_detached(ctx, [&](auto yield) {
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
            auto r = dht_node.bootstrap_single(
                    bs,
                    dht_node._cancel,
                    yield[ec]);
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

    auto metrics_client = metrics::Client();
    auto metrics_dht = metrics_client.mainline_dht();

    DhtNode dht_node(ctx, metrics_dht.dht_node_ipv4());

    init_without_bootstrapping(ctx, dht_node);
    bootstrap(ctx, dht_node);
    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()
