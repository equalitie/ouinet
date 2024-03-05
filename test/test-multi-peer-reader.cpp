#define BOOST_TEST_MODULE "Tests for cache/multi_peer_reader"

#include <boost/test/included/unit_test.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <utility>
#include <cache/multi_peer_reader.h>
#include <cache/local_peer_discovery.h>
#include <cache/dht_lookup.h>
#include <util/lru_cache.h>
#include "util/bittorrent_utils.cpp"
#include <bep5_swarms.h>

using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;
using namespace ouinet::util;

const std::string test_group = "ouinet.work";
const std::string dht_endpoint = "0.0.0.0";
const std::string debug_tag = "test-multi-peer-reader";
const std::string public_key = "zh6ylt6dghu6swhhje2j66icmjnonv53tstxxvj6acu64sc62fnq";
const std::string swarm_name = "ed25519:" + public_key + "/v6/uri/" + test_group;

using PeerLookup = ouinet::cache::DhtLookup;
using MultiPeerReader = ouinet::cache::MultiPeerReader;

asio::io_context tsuite_ctx;
BtUtils btu{tsuite_ctx};
std::shared_ptr<MainlineDht> btdht;
util::LruCache<std::string, shared_ptr<PeerLookup>> peer_lookups(256);

struct DhtFixture {
    DhtFixture() {
        asio::spawn(tsuite_ctx, [&](asio::yield_context yield) {
            vector<asio::ip::address> ifaddrs{asio::ip::make_address(dht_endpoint)};
            btdht = std::move(btu.bittorrent_dht(std::move(yield), ifaddrs));
        });
    }
    ~DhtFixture() = default;

    static util::Ed25519PublicKey pubkey(const std::string& pkey) {
        auto pk_s = util::base32_decode(pkey);
        assert(pk_s.size() == util::Ed25519PublicKey::key_size);
        auto pk_a = util::bytes::to_array<uint8_t, util::Ed25519PublicKey::key_size>(pk_s);
        return {pk_a};
    }
};

shared_ptr<PeerLookup> do_peer_lookup() {
    assert(btdht);

    auto* lookup = peer_lookups.get(swarm_name);

    if (!lookup) {
        lookup = peer_lookups.put( swarm_name
                , make_shared<PeerLookup>(btdht, swarm_name));
    }

    return *lookup;
}

BOOST_AUTO_TEST_SUITE(s, * boost::unit_test::fixture<DhtFixture>())

BOOST_AUTO_TEST_CASE(test_multi_peer_reader)
{
    std::set<udp::endpoint> _lan_my_endpoints;
    LocalPeerDiscovery _local_peer_discovery(tsuite_ctx.get_executor(), _lan_my_endpoints);
    std::shared_ptr<unsigned> newest_proto_seen;

    asio::spawn(tsuite_ctx, [&](const asio::yield_context& yield) {
        sys::error_code ec;
        asio::steady_timer timer(tsuite_ctx);
        auto peer_lookup = do_peer_lookup();
        auto reader = std::make_unique<MultiPeerReader>(
                tsuite_ctx.get_executor(),
                test_group, // is key different from group in this context
                DhtFixture::pubkey(public_key),
                _local_peer_discovery.found_peers(),
                btdht->local_endpoints(),
                btdht->wan_endpoints(),
                move(peer_lookup),
                newest_proto_seen, // init?
                debug_tag
            );
        BOOST_TEST(1);
        timer.expires_from_now(chrono::seconds(5));
        timer.async_wait(yield[ec]);
        raise(SIGINT);
    });

    boost::asio::signal_set signals(tsuite_ctx, SIGINT);
    signals.async_wait([&](const boost::system::error_code& error , int signal_number) {
        tsuite_ctx.stop();
    });
    tsuite_ctx.run();
}

BOOST_AUTO_TEST_SUITE_END();
