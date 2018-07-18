#define BOOST_TEST_MODULE bittorrent
#include <boost/test/included/unit_test.hpp>
#include <boost/optional.hpp>
#include <boost/asio.hpp>

#include <namespaces.h>
#include <iostream>

#define private public
#include <bittorrent/node_id.h>
#include <bittorrent/dht.h>
#include <bittorrent/code.h>
#include <bittorrent/byte_printer.h>

BOOST_AUTO_TEST_SUITE(bittorrent)

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;

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

static udp::endpoint as_udp_endpoint(tcp::endpoint ep) {
    return { ep.address(), ep.port() };
}

BOOST_AUTO_TEST_CASE(test_bep_5)
{
    using namespace ouinet::bittorrent::dht;

    asio::io_service ios;

    DhtNode dht(ios, asio::ip::make_address("0.0.0.0")); // TODO: IPv6

    asio::spawn(ios, [&] (auto yield) {
        sys::error_code ec;

        stringstream ss;
        NodeID infohash = util::sha1(ss.str());

        dht.start(yield[ec]);
        BOOST_REQUIRE(!ec);

        dht.tracker_announce(infohash, dht.wan_endpoint().port(), yield[ec]);
        BOOST_REQUIRE(!ec);

        auto peers = dht.tracker_get_peers(infohash , yield);

        bool found = false;

        for (auto p : peers) {
            if (as_udp_endpoint(p) == dht.wan_endpoint()) {
                found = true;
                break;
            }
        }

        BOOST_REQUIRE(found);

        dht.stop();
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
