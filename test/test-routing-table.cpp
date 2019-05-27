#define BOOST_TEST_MODULE routing_table
#include <boost/test/included/unit_test.hpp>
#include <set>

// Dirty trick to allow us inspect members of the RoutingTable class
#define private public

#include <bittorrent/routing_table.h>

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;

namespace std {
    template<class T>
    ostream& operator<<(ostream& os, const vector<T>& v) {
        os << "[";
        for (auto i = v.begin(); i != v.end(); ++i) {
            os << *i;
            if (next(i) != v.end()) os << ", ";
        }
        return os << "]";
    }
}

BOOST_AUTO_TEST_SUITE(ouinet_routing_table)


using udp = asio::ip::udp;
using boost::string_view;

udp::endpoint endpoint(const string& ip, uint16_t port)
{
    return udp::endpoint(asio::ip::address::from_string(ip), port);
}

template<class T>
bool unique(initializer_list<T> v)
{
    set<T> s;
    for (auto& e : v) { s.insert(e); }
    return v.size() == s.size();
}

NodeID from_bitstr(string_view s) {
    NodeID ret;
    for (size_t i = 0; i < NodeID::bit_size; ++i) {
        ret.set_bit(i, i < s.size() ? s[i] == '1' : rand() % 2 == 1);
    }
    return ret;
}

BOOST_AUTO_TEST_CASE(test_basics) {
    NodeID my_id = NodeID::Range::max().random_id();

    size_t pings_sent = 0;
    auto send_ping = [&] (NodeContact) { ++pings_sent; };

    RoutingTable rt(my_id, send_ping);

    BOOST_REQUIRE_EQUAL(rt._buckets.size(), 1u);
    BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 0u);

    NodeID id1 = NodeID::Range::max().random_id();

    BOOST_REQUIRE(unique({my_id, id1}));

    rt.try_add_node({ id1, endpoint("192.168.0.1", 5555) }, false);

    BOOST_REQUIRE_EQUAL(rt.find_bucket_id(id1), 0u);
    BOOST_REQUIRE_EQUAL(rt._buckets.size(), 1u);
    BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 0u);
    BOOST_REQUIRE_EQUAL(pings_sent, 1u);

    NodeID id2 = NodeID::Range::max().random_id();

    BOOST_REQUIRE(unique({my_id, id1, id2}));

    rt.try_add_node({ id2, endpoint("192.168.0.2", 5555) }, true);

    BOOST_REQUIRE_EQUAL(rt._buckets.size(), 1u);
    BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 1u);
    BOOST_REQUIRE_EQUAL(pings_sent, 1u);
}

BOOST_AUTO_TEST_CASE(test_split_bucket) {

    static const auto BUCKET_SIZE = RoutingTable::BUCKET_SIZE;

    {
        RoutingTable rt(from_bitstr("00000000000"), [&] (NodeContact) {});

        auto ip = "192.168.0.1";

        NodeContact cs[RoutingTable::BUCKET_SIZE + 1]
            = { { from_bitstr("111111111111"), endpoint(ip, 5000) }
              , { from_bitstr("101111111111"), endpoint(ip, 5001) }
              , { from_bitstr("110111111111"), endpoint(ip, 5002) }
              , { from_bitstr("100111111111"), endpoint(ip, 5003) }
              , { from_bitstr("100011111111"), endpoint(ip, 5004) }
              , { from_bitstr("100101111111"), endpoint(ip, 5005) }
              , { from_bitstr("100110011111"), endpoint(ip, 5006) }
              , { from_bitstr("100111001111"), endpoint(ip, 5007) }
              , { from_bitstr("100000000111"), endpoint(ip, 5008) } };

        for (size_t i = 0; i < sizeof(cs)/sizeof(*cs); ++i) {
            rt.try_add_node(cs[i], true);
        }

        auto ns = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                               , BUCKET_SIZE);

        BOOST_REQUIRE_EQUAL( ns.size(), BUCKET_SIZE);
        BOOST_REQUIRE_EQUAL( ns
                           , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                 , cs[4], cs[5], cs[6], cs[7] }));

        // Last one shouldn't be added
        BOOST_REQUIRE_EQUAL(rt._buckets.size(), 1u);
    }

    {
        RoutingTable rt(from_bitstr("00000000000"), [&] (NodeContact) {});

        auto ip = "192.168.0.1";

        NodeContact cs[BUCKET_SIZE + 1]
            = { { from_bitstr("111111111111"), endpoint(ip, 5000) }
              , { from_bitstr("101111111111"), endpoint(ip, 5001) }
              , { from_bitstr("110111111111"), endpoint(ip, 5002) }
              , { from_bitstr("100111111111"), endpoint(ip, 5003) }
              , { from_bitstr("100011111111"), endpoint(ip, 5004) }
              , { from_bitstr("100101111111"), endpoint(ip, 5005) }
              , { from_bitstr("100110011111"), endpoint(ip, 5006) }
              , { from_bitstr("100111001111"), endpoint(ip, 5007) }
              , { from_bitstr("000000000111"), endpoint(ip, 5008) } };

        for (size_t i = 0; i < sizeof(cs)/sizeof(*cs); ++i) {
            rt.try_add_node(cs[i], true);
        }

        BOOST_REQUIRE_EQUAL(rt._buckets.size(), 2u);
        BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), BUCKET_SIZE);
        BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), 1u);

        auto ns1 = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                               , BUCKET_SIZE);

        BOOST_REQUIRE_EQUAL( ns1
                           , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                 , cs[4], cs[5], cs[6], cs[7] }));

        auto ns2 = rt.find_closest_routing_nodes( from_bitstr("0000000000")
                                               , BUCKET_SIZE);

        BOOST_REQUIRE_EQUAL( ns2
                           , vector<NodeContact>({ cs[8], cs[0], cs[1], cs[2]
                                                 , cs[3], cs[4], cs[5], cs[6] }));
    }

    {
        RoutingTable rt(from_bitstr("00000"), [&] (NodeContact) {});

        auto ip = "192.168.0.1";

        NodeContact cs[BUCKET_SIZE + 1]
            = { { from_bitstr("10000"), endpoint(ip, 5000) }
              , { from_bitstr("10001"), endpoint(ip, 5001) }
              , { from_bitstr("10010"), endpoint(ip, 5002) }
              , { from_bitstr("10011"), endpoint(ip, 5003) }

              , { from_bitstr("00001"), endpoint(ip, 5004) }
              , { from_bitstr("00011"), endpoint(ip, 5005) }
              , { from_bitstr("00101"), endpoint(ip, 5006) }
              , { from_bitstr("00111"), endpoint(ip, 5007) }

              , { from_bitstr("10100"), endpoint(ip, 5008) } };

        for (size_t i = 0; i < sizeof(cs)/sizeof(*cs); ++i) {
            rt.try_add_node(cs[i], true);
        }

        BOOST_REQUIRE_EQUAL(rt._buckets.size(), 2u);
        BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 5u);
        BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), 4u);

        auto ns1 = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                                , BUCKET_SIZE);

        BOOST_REQUIRE_EQUAL( ns1
                           , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                 , cs[8], cs[4], cs[5], cs[6] }));

    }

    {
        RoutingTable rt(from_bitstr("00000000000"), [&] (NodeContact) {});

        NodeID ids[BUCKET_SIZE + 1]
            = { from_bitstr("011111111111")
              , from_bitstr("001111111111")
              , from_bitstr("010111111111")
              , from_bitstr("000111111111")
              , from_bitstr("000011111111")
              , from_bitstr("000101111111")
              , from_bitstr("000110011111")
              , from_bitstr("000111001111")
              , from_bitstr("100000000111") };

        for (size_t i = 0; i < sizeof(ids)/sizeof(*ids); ++i) {
            rt.try_add_node({ ids[i], endpoint("192.168.0.1", 5555 + i) }, true);
        }

        BOOST_REQUIRE_EQUAL(rt._buckets.size(), 2u);
        BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 1u);
        BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), BUCKET_SIZE);
    }

    for (unsigned k = 0; k < 2000; ++k) {
        NodeID my_id = NodeID::Range::max().random_id();

        auto send_ping = [&] (NodeContact) { };

        RoutingTable rt(my_id, send_ping);

        for (size_t i = 0; i < BUCKET_SIZE + 1; ++i) {
            NodeID new_id = NodeID::Range::max().random_id();
            rt.try_add_node({ new_id, endpoint("192.168.0.1", 5555 + i) }, true);
        }
    }
}

BOOST_AUTO_TEST_CASE(test_three_buckets_add_to_middle) {

    static const auto BUCKET_SIZE = RoutingTable::BUCKET_SIZE;

    {
        RoutingTable rt(from_bitstr("00000"), [&] (NodeContact) {});

        auto ip = "192.168.0.1";

        NodeContact cs[]
            = { { from_bitstr("100000"), endpoint(ip, 5000) }
              , { from_bitstr("100001"), endpoint(ip, 5001) }
              , { from_bitstr("100010"), endpoint(ip, 5002) }
              , { from_bitstr("100011"), endpoint(ip, 5003) }
              , { from_bitstr("100100"), endpoint(ip, 5004) }
              , { from_bitstr("100101"), endpoint(ip, 5005) }
              , { from_bitstr("100110"), endpoint(ip, 5006) }
              , { from_bitstr("100111"), endpoint(ip, 5007) }

              , { from_bitstr("001000"), endpoint(ip, 5008) }
              , { from_bitstr("001001"), endpoint(ip, 5009) }
              , { from_bitstr("001010"), endpoint(ip, 5010) }
              , { from_bitstr("001011"), endpoint(ip, 5011) }
              , { from_bitstr("001100"), endpoint(ip, 5012) }
              , { from_bitstr("001101"), endpoint(ip, 5013) }
              , { from_bitstr("001110"), endpoint(ip, 5014) }
              , { from_bitstr("001111"), endpoint(ip, 5015) } };

        for (size_t i = 0; i < sizeof(cs)/sizeof(*cs); ++i) {
            rt.try_add_node(cs[i], true);
        }

        {
            BOOST_REQUIRE_EQUAL(rt._buckets.size(), 2u);
            BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 8u);
            BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), 8u);

            auto ns1 = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                                    , BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns1
                               , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                     , cs[4], cs[5], cs[6], cs[7] }));
        }

        NodeContact c { from_bitstr("0100"), endpoint(ip, 5016) };

        rt.try_add_node(c, true);

        BOOST_REQUIRE_EQUAL(rt._buckets.size(), 3u);
        BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 8u);
        BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), 1u);
        BOOST_REQUIRE_EQUAL(rt._buckets[2].nodes.size(), 8u);

        {
            auto ns = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                                   , BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns
                               , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                     , cs[4], cs[5], cs[6], cs[7] }));
        }

        {
            auto ns = rt.find_closest_routing_nodes( from_bitstr("00000000")
                                                    , BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns
                               , vector<NodeContact>({ cs[8],  cs[9], cs[10], cs[11]
                                                     , cs[12], cs[13], cs[14], cs[15] }));
        }

        {
            auto ns = rt.find_closest_routing_nodes(c.id, BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns
                               , vector<NodeContact>({ c,  cs[8], cs[9], cs[10]
                                                     , cs[11], cs[12], cs[13], cs[14] }));
        }
    }
}

BOOST_AUTO_TEST_CASE(test_three_buckets_add_to_end) {

    static const auto BUCKET_SIZE = RoutingTable::BUCKET_SIZE;

    {
        RoutingTable rt(from_bitstr("00000"), [&] (NodeContact) {});

        auto ip = "192.168.0.1";

        NodeContact cs[]
            = { { from_bitstr("100000"), endpoint(ip, 5000) }
              , { from_bitstr("100001"), endpoint(ip, 5001) }
              , { from_bitstr("100010"), endpoint(ip, 5002) }
              , { from_bitstr("100011"), endpoint(ip, 5003) }
              , { from_bitstr("100100"), endpoint(ip, 5004) }
              , { from_bitstr("100101"), endpoint(ip, 5005) }
              , { from_bitstr("100110"), endpoint(ip, 5006) }
              , { from_bitstr("100111"), endpoint(ip, 5007) }

              , { from_bitstr("010000"), endpoint(ip, 5008) }
              , { from_bitstr("010001"), endpoint(ip, 5009) }
              , { from_bitstr("010010"), endpoint(ip, 5010) }
              , { from_bitstr("010011"), endpoint(ip, 5011) }
              , { from_bitstr("010100"), endpoint(ip, 5012) }
              , { from_bitstr("010101"), endpoint(ip, 5013) }
              , { from_bitstr("010110"), endpoint(ip, 5014) }
              , { from_bitstr("010111"), endpoint(ip, 5015) } };

        for (size_t i = 0; i < sizeof(cs)/sizeof(*cs); ++i) {
            rt.try_add_node(cs[i], true);
        }

        {
            BOOST_REQUIRE_EQUAL(rt._buckets.size(), 2u);
            BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 8u);
            BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), 8u);

            auto ns1 = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                                    , BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns1
                               , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                     , cs[4], cs[5], cs[6], cs[7] }));
        }

        NodeContact c { from_bitstr("0001"), endpoint(ip, 5016) };

        rt.try_add_node(c, true);

        BOOST_REQUIRE_EQUAL(rt._buckets.size(), 3u);
        BOOST_REQUIRE_EQUAL(rt._buckets[0].nodes.size(), 8u);
        BOOST_REQUIRE_EQUAL(rt._buckets[1].nodes.size(), 8u);
        BOOST_REQUIRE_EQUAL(rt._buckets[2].nodes.size(), 1u);

        {
            auto ns = rt.find_closest_routing_nodes( from_bitstr("11111111")
                                                   , BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns
                               , vector<NodeContact>({ cs[0], cs[1], cs[2], cs[3]
                                                     , cs[4], cs[5], cs[6], cs[7] }));
        }

        {
            auto ns = rt.find_closest_routing_nodes( from_bitstr("00000000")
                                                    , BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns
                               , vector<NodeContact>({ c,     cs[8],  cs[9], cs[10]
                                                     , cs[11], cs[12], cs[13], cs[14] }));
        }

        {
            auto ns = rt.find_closest_routing_nodes(cs[8].id, BUCKET_SIZE);

            BOOST_REQUIRE_EQUAL( ns
                               , vector<NodeContact>({ cs[8],  cs[9], cs[10], cs[11]
                                                     , cs[12], cs[13], cs[14], cs[15] }));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
