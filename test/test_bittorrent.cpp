#define BOOST_TEST_MODULE bittorrent
#include <boost/test/included/unit_test.hpp>
#include <boost/optional.hpp>
#include <boost/asio.hpp>

#include <namespaces.h>
#include <iostream>
#include <util/wait_condition.h>

#define private public
#include <bittorrent/dht_node.h>
#include <bittorrent/code.h>
#include <util/hash.h>

#include "constants.h"
#include "task.h"
#include "cxx/metrics.h"

namespace utf = boost::unit_test;

BOOST_AUTO_TEST_SUITE(bittorrent, * utf::timeout(240))

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using Clock = chrono::steady_clock;

using boost::optional;

BOOST_AUTO_TEST_CASE(test_generate_node_id)
{
    // The first test vector from here:
    // http://bittorrent.org/beps/bep_0042.html#node-id-restriction
    //
    // Note that the test vectors differ very slightly because bytes
    // 22 to 24 are actually random.

    auto ip = boost::asio::ip::make_address_v4("124.31.75.21");
    auto id = NodeID::generate(ip, 1);

    // Setting bytes 22-24 to zero as according to BEP42 only the first
    // 21 bits are expected to match in the resulting hash.
    for (uint8_t b = 21; b < 24; ++b)
        id.set_bit(b, false);
    auto id_str = id.to_hex();

    BOOST_REQUIRE_EQUAL(id_str.substr(0, 6), "5fbfb8");
    BOOST_REQUIRE_EQUAL(id_str.substr(38), "01");
}

static
__attribute__((unused))
float seconds(Clock::duration d)
{
    using namespace chrono;
    return duration_cast<milliseconds>(d).count() / 1000.f;
}

BOOST_AUTO_TEST_CASE(test_bep_5,
                     * utf::timeout(240))
{
    using namespace ouinet::bittorrent;

    asio::io_context ctx;

    auto metrics_client = metrics::Client();
    auto metrics_dht = metrics_client.mainline_dht();
    bool do_doh = true;
    uint32_t rx_limit = default_udp_mux_rx_limit;

    DhtNode dht(ctx.get_executor(), metrics_dht.dht_node_ipv4(), do_doh, rx_limit);

    task::spawn_detached(ctx, [&] (auto yield) {
        sys::error_code ec;
        Signal<void()> cancel_signal;

        NodeID infohash = util::sha1_digest("ouinet-test-" + to_string(time(0)));

        dht.start({asio::ip::make_address("0.0.0.0"), 0}, yield[ec]); // TODO: IPv6

        asio::steady_timer timer(dht.get_executor());
        while (!ec && !dht.ready()) {
            timer.expires_after(chrono::milliseconds(200));
            timer.async_wait(yield[ec]);
        }
        BOOST_REQUIRE(!ec);

        dht.tracker_announce(infohash, dht.wan_endpoint().port(), cancel_signal, yield[ec]);
        BOOST_REQUIRE(!ec);

        auto peers = dht.tracker_get_peers(infohash , cancel_signal, yield[ec]);
        BOOST_REQUIRE(!ec);

        BOOST_REQUIRE(peers.count(dht.wan_endpoint()));

        dht.stop();
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_bep_44,
                     * utf::disabled()
                     * utf::description("tests unused feature, fails randomly in CI"))
{
    using namespace ouinet::bittorrent;

    asio::io_context ctx;

    auto metrics_client = metrics::Client();
    auto metrics_dht = metrics_client.mainline_dht();
    bool do_doh = true;
    uint32_t rx_limit = default_udp_mux_rx_limit;

    DhtNode dht(ctx.get_executor(), metrics_dht.dht_node_ipv4(), do_doh, rx_limit);

    auto mutable_data = []( const string& value
                          , const string& salt
                          , const util::Ed25519PrivateKey& private_key)
    {
        // Use the timestamp as a version ID.
        using Time = boost::posix_time::ptime;

        Time unix_epoch(boost::gregorian::date(1970, 1, 1));
        Time ts = boost::posix_time::microsec_clock::universal_time();

        return MutableDataItem::sign( value
                                    , (ts - unix_epoch).total_milliseconds()
                                    , salt
                                    , private_key);
    };

    sys::error_code ec;
    Cancel cancel;

    auto skey = util::Ed25519PrivateKey::generate();
    auto pkey = skey.public_key();

    size_t push_get_count = 8;
    size_t success_count = 0;

    task::spawn_detached(ctx, [&] (auto yield) {
        dht.start({asio::ip::make_address("0.0.0.0"), 0}, yield[ec]); // TODO: IPv6

        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE(dht.ready());

        WaitCondition wc(ctx);

        for (size_t i = 0; i < push_get_count; i++) {
        task::spawn_detached(ctx, [&, lock = wc.lock(), i] (auto yield) {
                BOOST_REQUIRE(!ec);

                stringstream salt;
                stringstream value;
                salt  << "salt-" << i;
                value << "value-" << i;

                //cerr << "Start " << value.str() << endl;
                auto item = mutable_data(value.str(), salt.str(), skey);

                //auto start = Clock::now();
                dht.data_put_mutable(item, cancel, yield[ec]);
                //cerr << "Putting data " << value.str()
                //     << " took " << seconds(Clock::now() - start) << "s"
                //     << endl;

                BOOST_CHECK(!ec);
                if (ec) return;

                //start = Clock::now();
                auto opt_data = dht.data_get_mutable( pkey
                                                    , salt.str()
                                                    , cancel
                                                    , yield[ec]);
                //cerr << "Getting data " << value.str()
                //     << " took " << seconds(Clock::now() - start) << "s"
                //     << endl;

                BOOST_CHECK(!ec);
                BOOST_CHECK(opt_data);

                if (opt_data) {
                    auto s = opt_data->value.as_string();
                    BOOST_REQUIRE(s);
                    BOOST_REQUIRE(value.str() == *s);
                    success_count++;
                }
            });
        }

        wc.wait(yield[ec]);
        BOOST_REQUIRE(!ec);

        dht.stop();
    });

    ctx.run();

    BOOST_REQUIRE_EQUAL(push_get_count, success_count);
}

BOOST_AUTO_TEST_SUITE_END()
