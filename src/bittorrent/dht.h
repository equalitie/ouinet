#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <vector>
#include <set>

#include "bencoding.h"
#include "dht_storage.h"
#include "mutable_data.h"
#include "node_id.h"
#include "routing_table.h"
#include "contact.h"

#include "../namespaces.h"
#include "../util/crypto.h"
#include "../util/signal.h"
#include "../util/wait_condition.h"

namespace ouinet {
namespace bittorrent {

class UdpMultiplexer;

asio::ip::udp::endpoint resolve(
    asio::io_context& ioc,
    const std::string& addr,
    const std::string& port,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
);

namespace ip = asio::ip;
using ip::tcp;
using ip::udp;


namespace dht {

/**
 * To ensure that cancellation and object destruction behave in a predictable
 * way, all functions in this namespace follow the following invariant:
 *
 * Every function with a yield parameter and a cancel signal MUST report
 * asio::error::operation_aborted if the cancel signal is called while the
 * function is still on the stack, even if the operation has successfully
 * completed in the meantime.
 *
 *   This requirement is trivial as long as all asynchronous operations are
 *   calls to foreground coroutine functions with a cancel signal, and no
 *   additional coroutines are spawned. It takes special attention otherwise.
 *
 * Every method with a yield parameter MUST report
 * asio::error::operation_aborted if the object is destructed while the
 * method is still on the stack, even if the operation has successfully
 * completed in the meantime.
 *
 *   This requirement is trivial as long as all asynchronous operations are
 *   calls to coroutine methods in the same object, or calls to coroutine
 *   methods in member objects. It takes special attention otherwise.
 */

class DhtNode {
    public:
    const size_t RESPONSIBLE_TRACKERS_PER_SWARM = 8;

    public:
    DhtNode(asio::io_service& ios, ip::address interface_address);
    void start(asio::yield_context yield);
    void stop();

    /**
     * True iff this DhtNode knows enough about the structure of the DHT to
     * reliably submit queries to it. The DHT operations below may be called
     * only when the DhtNode is ready(). The DhtNode will be ready() when
     * start() completes.
     */
    bool ready() const { return _ready; }

    /**
     * Query peers for a bittorrent swarm surrounding a particular infohash.
     * This returns a randomized subset of all such peers, not the entire swarm.
     */
    std::set<tcp::endpoint> tracker_get_peers(
        NodeID infohash,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    /**
     * Announce yourself on the bittorrent swarm surrounding a particular
     * infohash, and retrieve existing peers in that swarm.
     * This returns a randomized subset of all such peers, not the entire swarm.
     *
     * @param port If set, announce yourself on the TCP (and, possibly, UDP)
     *     port listed. If unset, announce yourself on the UDP (and, possibly,
     *     TCP) port used for communicating with the DHT.
     *
     * TODO: [ruud] I am not clear to what degree this is actually followed in practice.
     */
    std::set<tcp::endpoint> tracker_announce(
        NodeID infohash,
        boost::optional<int> port,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    /**
     * Search the DHT for BEP-44 immutable data item with key $key.
     * @return The data stored in the DHT under $key, or boost::none if no such
     *         data was found.
     */
    boost::optional<BencodedValue> data_get_immutable(
        const NodeID& key,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    /**
     * Store $data in the DHT as a BEP-44 immutable data item.
     * @return The ID as which this data is known in the DHT, equal to the
     *         sha1 hash of the bencoded $data.
     */
    NodeID data_put_immutable(
        const BencodedValue& data,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    /**
     * Search the DHT for BEP-44 mutable data item with a given (public key, salt)
     * combination.
     * @return The data stored in the DHT under ($public_key, $salt), or
     *         boost::none if no such data was found.
     *
     * TODO: Implement minimum sequence number if we ever need it.
     */
    boost::optional<MutableDataItem> data_get_mutable(
        const util::Ed25519PublicKey& public_key,
        boost::string_view salt,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    /**
     * Store a pre-signed BEP-44 mutable data item in the DHT. The data item
     * can be found when searching for the combination of (public key, salt).
     *
     * @return The ID as which this data is known in the DHT.
     *
     * TODO: Implement compare-and-swap if we ever need it.
     */
    NodeID data_put_mutable(
        MutableDataItem data,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    /**
     * Store $data in the DHT as a BEP-44 mutable data item. The data item
     * can be found when searching for the combination of (public key, salt).
     *
     * @param private_key The private key whose public key identifies the data item.
     * @param salt The salt which identifies the data item. May be empty.
     * @param sequence_number Version number of the data item. Must be larger
     *            than any previous version number used for this data item.
     * @return The ID as which this data is known in the DHT.
     *
     * TODO: Implement compare-and-swap if we ever need it.
     */
    NodeID data_put_mutable(
        const BencodedValue& data,
        const util::Ed25519PrivateKey& private_key,
        const std::string& salt,
        int64_t sequence_number,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    // http://bittorrent.org/beps/bep_0005.html#ping
    BencodedMap send_ping(
        NodeContact contact,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );
    void send_ping(NodeContact contact);

    // http://bittorrent.org/beps/bep_0005.html#find-node
    bool query_find_node(
        NodeID target_id,
        Contact node,
        std::vector<NodeContact>& closer_nodes,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    bool is_v4() const { return _interface_address.is_v4(); }
    bool is_v6() const { return _interface_address.is_v6(); }

    udp::endpoint wan_endpoint() const { return _wan_endpoint; }

    ~DhtNode();

    private:
    void receive_loop(asio::yield_context yield);

    void send_datagram(
        udp::endpoint destination,
        const BencodedMap& query_arguments
    );
    void send_datagram(
        udp::endpoint destination,
        const BencodedMap& query_arguments,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    void send_query(
        udp::endpoint destination,
        std::string transaction,
        std::string query_type,
        BencodedMap query_arguments,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    BencodedMap send_query_await_reply(
        Contact,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        asio::steady_timer::duration timeout,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    void handle_query(udp::endpoint sender, BencodedMap query);

    void bootstrap(asio::yield_context yield);

    void refresh_routing_table();

    std::vector<NodeContact> find_closest_nodes(
        NodeID target_id,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    std::string new_transaction_string();

    void send_write_query(
        udp::endpoint destination,
        NodeID destination_id,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        asio::yield_context,
        Signal<void()>& cancel_signal
    );

    // http://bittorrent.org/beps/bep_0005.html#get-peers
    boost::optional<BencodedMap> query_get_peers(
        NodeID infohash,
        Contact node,
        std::vector<NodeContact>& closer_nodes,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    // http://bittorrent.org/beps/bep_0044.html#get-message
    boost::optional<BencodedMap> query_get_data(
        NodeID key,
        Contact node,
        std::vector<NodeContact>& closer_nodes,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );


    struct TrackerNode {
        asio::ip::udp::endpoint node_endpoint;
        std::string announce_token;
    };
    void tracker_do_search_peers(
        NodeID infohash,
        std::set<tcp::endpoint>& peers,
        std::map<NodeID, TrackerNode>& responsible_nodes,
        asio::yield_context,
        Signal<void()>& cancel_signal
    );

    void routing_bucket_try_add_node(
        RoutingBucket*,
        NodeContact,
        bool is_verified
    );

    void routing_bucket_fail_node(RoutingBucket*, NodeContact);

    static bool closer_to(const NodeID& reference, const NodeID& left, const NodeID& right);

    template<class Evaluate>
    void collect(
        const NodeID& target,
        Evaluate&&,
        asio::yield_context,
        Signal<void()>& cancel_signal
    );

    private:
    asio::io_service& _ios;
    ip::address _interface_address;
    std::unique_ptr<UdpMultiplexer> _multiplexer;
    NodeID _node_id;
    bool _initialized;
    udp::endpoint _wan_endpoint;
    std::unique_ptr<RoutingTable> _routing_table;
    std::unique_ptr<Tracker> _tracker;
    std::unique_ptr<DataStore> _data_store;
    bool _ready;
    Signal<void()> _terminate_signal;

    struct ActiveRequest {
        udp::endpoint destination;
        std::function<void(const BencodedMap&)> callback;
    };
    uint32_t _next_transaction_id;
    std::map<std::string, ActiveRequest> _active_requests;

    std::vector<udp::endpoint> _bootstrap_endpoints;

    std::pair<asio::ip::udp::endpoint, asio::ip::udp::endpoint> bootstrap_single(asio::yield_context yield, std::string bootstrap_domain);
};

struct DhtPublications
{
    /*
     * There does not seem to be any spec for this. 20 minute based on
     * what other implementations seem to do.
     */
    const int ANNOUNCE_INTERVAL_SECONDS = 60 * 20;
    /*
     * http://www.bittorrent.org/beps/bep_0044.html#expiration recommends
     * republish every hour, and expiring after two hours. We'll republish
     * slightly faster, to avoid unfortunate rounding errors.
     */
    const int PUT_INTERVAL_SECONDS = 60 * 50;

    struct TrackerPublication {
        boost::optional<int> port;
        std::chrono::steady_clock::time_point last_sent;
    };
    std::map<NodeID, TrackerPublication> tracker_publications;

    struct ImmutablePublication {
        BencodedValue data;
        std::chrono::steady_clock::time_point last_sent;
    };
    std::map<NodeID, ImmutablePublication> immutable_publications;

    struct MutablePublication {
        MutableDataItem data;
        std::chrono::steady_clock::time_point last_sent;
    };
    std::map<NodeID, MutablePublication> mutable_publications;
};

} // dht namespace

class MainlineDht {
    public:
    MainlineDht(asio::io_service& ios);

    MainlineDht(const MainlineDht&) = delete;
    MainlineDht& operator=(const MainlineDht&) = delete;

    ~MainlineDht();

    void set_interfaces(const std::vector<asio::ip::address>& addresses);

    /*
     * When cancelled, the publication still goes through and will be refreshed
     * until _stop()ped, but the _start() will not wait for successful completion.
     */
    /*
     * TODO: announce() and put() functions don't have any real error detection.
     */
    std::set<tcp::endpoint> tracker_announce_start(
        NodeID infohash,
        boost::optional<int> port,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );
    std::set<tcp::endpoint> tracker_announce_start(
        NodeID infohash,
        boost::optional<int> port,
        asio::yield_context yield
    )
        { Signal<void()> cancel_signal; return tracker_announce_start(infohash, port, yield, cancel_signal); }
    void tracker_announce_start(
        NodeID infohash,
        boost::optional<int> port
    );
    void tracker_announce_stop(NodeID infohash);

    NodeID immutable_put_start(
        const BencodedValue& data,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );
    NodeID immutable_put_start(const BencodedValue& data, asio::yield_context yield)
        { Signal<void()> cancel_signal; return immutable_put_start(data, yield, cancel_signal); }
    NodeID immutable_put_start(const BencodedValue& data);
    void immutable_put_stop(NodeID key);

    void mutable_put(const MutableDataItem&, Cancel&, asio::yield_context);

    NodeID mutable_put_start(
        const MutableDataItem& data,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );
    NodeID mutable_put_start(const MutableDataItem& data, asio::yield_context yield)
        { Signal<void()> cancel_signal; return mutable_put_start(data, yield, cancel_signal); }
    NodeID mutable_put_start(const MutableDataItem& data);
    void mutable_put_stop(NodeID key);

    std::set<tcp::endpoint> tracker_get_peers(NodeID infohash, asio::yield_context yield, Signal<void()>& cancel_signal);
    std::set<tcp::endpoint> tracker_get_peers(NodeID infohash, asio::yield_context yield)
        { Signal<void()> cancel_signal; return tracker_get_peers(infohash, yield, cancel_signal); }
    boost::optional<BencodedValue> immutable_get(NodeID key, asio::yield_context yield, Signal<void()>& cancel_signal);
    boost::optional<BencodedValue> immutable_get(NodeID key, asio::yield_context yield)
        { Signal<void()> cancel_signal; return immutable_get(key, yield, cancel_signal); }
    /*
     * TODO:
     *
     * Ideally, this interface should provide some way for the user to signal
     * when the best result found so far is good (that is, recent) enough, and
     * when to keep searching in the hopes of finding a more recent entry.
     * The current version is a quick-and-dirty good-enough-for-now.
     */
    boost::optional<MutableDataItem> mutable_get(
        const util::Ed25519PublicKey& public_key,
        boost::string_view salt,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );
    boost::optional<MutableDataItem> mutable_get(
        const util::Ed25519PublicKey& public_key,
        boost::string_view salt,
        asio::yield_context yield
    )
        { Signal<void()> cancel_signal; return mutable_get(public_key, salt, yield, cancel_signal); }

    asio::io_service& get_io_service() { return _ios; }

    bool all_ready() const {
        for (const auto& n : _nodes) {
            if (!n.second->ready()) return false;
        }
        return true;
    }
    void wait_all_ready(
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );

    private:
    asio::io_service& _ios;
    std::map<asio::ip::address, std::unique_ptr<dht::DhtNode>> _nodes;
    dht::DhtPublications _publications;
    Signal<void()> _terminate_signal;
};

} // bittorrent namespace
} // ouinet namespace
