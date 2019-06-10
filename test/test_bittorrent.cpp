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

BOOST_AUTO_TEST_SUITE(bittorrent)

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
    // Note though that the other test vectors differ very slightly.
    // I think that is a bug in bittorrent's documentation because
    // even their own reference implementation does differ.

    auto ip = boost::asio::ip::address_v4::from_string("124.31.75.21");
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

static
__attribute__((unused))
tcp::endpoint as_tcp(udp::endpoint ep) {
    return { ep.address(), ep.port() };
}

BOOST_AUTO_TEST_CASE(test_bep_5)
{
    using namespace ouinet::bittorrent::dht;

    asio::io_service ios;

    DhtNode dht(ios);

    asio::spawn(ios, [&] (auto yield) {
        sys::error_code ec;
        Signal<void()> cancel_signal;

        NodeID infohash = util::sha1("ouinet-test-" + to_string(time(0)));

        dht.start({asio::ip::make_address("0.0.0.0"), 0}, yield[ec]); // TODO: IPv6
        BOOST_REQUIRE(!ec);

        dht.tracker_announce(infohash, dht.wan_endpoint().port(), cancel_signal, yield[ec]);
        BOOST_REQUIRE(!ec);

        auto peers = dht.tracker_get_peers(infohash , cancel_signal, yield[ec]);
        BOOST_REQUIRE(!ec);

        BOOST_REQUIRE(peers.count(as_tcp(dht.wan_endpoint())));

        dht.stop();
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_bep_44)
{
    using namespace ouinet::bittorrent::dht;

    asio::io_service ios;

    DhtNode dht(ios);

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

    size_t push_get_count = 15;
    size_t success_count = 0;

    asio::spawn(ios, [&] (auto yield) {
        dht.start({asio::ip::make_address("0.0.0.0"), 0}, yield[ec]); // TODO: IPv6

        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE(dht.ready());

        WaitCondition wc(ios);

        for (size_t i = 0; i < push_get_count; i++) {
            asio::spawn(ios, [&, lock = wc.lock(), i] (auto yield) {
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

    ios.run();

    BOOST_REQUIRE_EQUAL(push_get_count, success_count);
}

BOOST_AUTO_TEST_SUITE_END()
