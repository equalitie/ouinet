#define BOOST_TEST_MODULE bittorrent
#include <boost/test/included/unit_test.hpp>
#include <boost/optional.hpp>
#include <boost/asio.hpp>

#include <namespaces.h>
#include <iostream>

#define private public
#include <bittorrent/node_id.h>

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

BOOST_AUTO_TEST_SUITE_END()
