#define BOOST_TEST_MODULE bittorrent
#include <boost/test/included/unit_test.hpp>
#include <boost/optional.hpp>
#include <boost/asio.hpp>

#include <namespaces.h>
#include <iostream>
#include <util/wait_condition.h>

#define private public
#include <bittorrent/node_id.h>
#include <bittorrent/dht.h>
#include <bittorrent/code.h>
#include <util/hash.h>

BOOST_AUTO_TEST_SUITE(bittorrent)

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using Clock = chrono::steady_clock;

using boost::optional;

namespace utf = boost::unit_test;

BOOST_AUTO_TEST_CASE(test_generate_node_id)
{
    // The first test vector from here:
    // http://bittorrent.org/beps/bep_0042.html#node-id-restriction
    //
    // Note though that the other test vectors differ very slightly.
    // I think that is a bug in bittorrent's documentation because
    // even their own reference implementation does differ.

    auto ip = boost::asio::ip::make_address_v4("124.31.75.21");
    auto id = NodeID::generate(ip, 1).to_hex();
    BOOST_REQUIRE_EQUAL(id.substr(0, 6), "5fbfbf");
    BOOST_REQUIRE_EQUAL(id.substr(38), "01");
}

static
__attribute__((unused))
float seconds(Clock::duration d)
{
    using namespace chrono;
    return duration_cast<milliseconds>(d).count() / 1000.f;
}

BOOST_AUTO_TEST_CASE(test_bep_5)
{
    using namespace ouinet::bittorrent::dht;

    asio::io_context ctx;

    DhtNode dht(ctx);

    asio::spawn(ctx, [&] (auto yield) {
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
    using namespace ouinet::bittorrent::dht;

    asio::io_context ctx;

    DhtNode dht(ctx);

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

    asio::spawn(ctx, [&] (auto yield) {
        dht.start({asio::ip::make_address("0.0.0.0"), 0}, yield[ec]); // TODO: IPv6

        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE(dht.ready());

        WaitCondition wc(ctx);

        for (size_t i = 0; i < push_get_count; i++) {
            asio::spawn(ctx, [&, lock = wc.lock(), i] (auto yield) {
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
