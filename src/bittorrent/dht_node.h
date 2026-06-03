#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/filesystem/path.hpp>

#include <chrono>
#include <type_traits>
#include <vector>
#include <set>

#include <asio_utp/udp_multiplexer.hpp>

#include "bencoding.h"
#include "bootstrap.h"
#include "dht_storage.h"
#include "mutable_data.h"
#include "node_id.h"
#include "routing_table.h"
#include "contact.h"
#include "cxx/dns.h"
#include "cxx/metrics.h"
#include "dht.h"
#include "api.h"

#include "../namespaces.h"
#include "../util/async.h"
#include "../util/sign.h"
#include "../util/cancel.h"
#include "../util/wait_condition.h"
#include "../util/async_queue.h"
#include "../util/watch_dog.h"

namespace ouinet::bittorrent {

class UdpMultiplexer;

namespace ip = asio::ip;
using ip::udp;

class DebugCtx;

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

class OUINET_COMMON_API DhtNode {
    public:
    const size_t RESPONSIBLE_TRACKERS_PER_SWARM = 8;

    public:
    DhtNode( const AsioExecutor&
           , metrics::DhtNode
           , std::shared_ptr<dns::Resolver>
           , uint32_t mux_rx_limit
           , boost::filesystem::path storage_dir
           , bootstrap::Config bs
           , util::LogPath
    );

    std::expected<void, sys::error_code> start(udp::endpoint, Async yield);
    std::expected<void, sys::error_code> start(asio_utp::udp_multiplexer, Async yield);
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
    std::expected<std::set<udp::endpoint>, sys::error_code> tracker_get_peers(
        NodeID infohash,
        Async
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
    std::expected<std::set<udp::endpoint>, sys::error_code> tracker_announce(
        NodeID infohash,
        std::optional<int> port,
        Async
    );

    /**
     * Search the DHT for BEP-44 immutable data item with key $key.
     * @return The data stored in the DHT under $key, or nullopt if no such
     *         data was found.
     */
    std::optional<BencodedValue> data_get_immutable(const NodeID& key, Async);

    /**
     * Store $data in the DHT as a BEP-44 immutable data item.
     * @return The ID as which this data is known in the DHT, equal to the
     *         sha1 hash of the bencoded $data.
     */
    NodeID data_put_immutable(
        const BencodedValue& data,
        Cancel&,
        asio::yield_context
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
        const sign::PublicKey& public_key,
        boost::string_view salt,
        Cancel&,
        asio::yield_context
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
        Cancel&,
        asio::yield_context
    );

    // http://bittorrent.org/beps/bep_0005.html#ping
    std::expected<BencodedMap, sys::error_code> send_ping(
        NodeContact contact,
        Async
    );

    void send_ping(NodeContact contact);

    // http://bittorrent.org/beps/bep_0005.html#find-node
    bool query_find_node(
        NodeID target_id,
        Contact node,
        std::vector<NodeContact>& closer_nodes,
        Async
    );

    std::expected<bool, sys::error_code> query_find_node2(
        NodeID target_id,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dead_man_switch,
        DebugCtx* dbg,
        Async
    );

    // http://bittorrent.org/beps/bep_0005.html#get-peers
    std::optional<BencodedMap> query_get_peers(
        NodeID infohash,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dms,
        DebugCtx* dbg,
        Async
    );

    bool is_v4() const { return _local_endpoint.address().is_v4(); }
    bool is_v6() const { return _local_endpoint.address().is_v6(); }

    udp::endpoint local_endpoint() const { return _local_endpoint; }
    udp::endpoint wan_endpoint() const { return _wan_endpoint; }

    ~DhtNode();

    AsioExecutor get_executor() { return _exec; }

    NodeID node_id() const { return _node_id; }

    private:
    void receive_loop(Async);
    void store_contacts_loop(Async);

    std::expected<void, sys::error_code> send_datagram(
        udp::endpoint destination,
        const BencodedMap& query_arguments,
        Async
    );

    std::expected<void, sys::error_code> send_query(
        udp::endpoint destination,
        std::string transaction,
        std::string query_type,
        BencodedMap query_arguments,
        Async
    );

    std::expected<BencodedMap, sys::error_code> send_query_await_reply(
        Contact,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        WatchDog* dms,
        DebugCtx* dbg,
        Async
    );

    void handle_query(udp::endpoint sender, BencodedMap& query, Cancel cancel, asio::yield_context);

    std::expected<void, sys::error_code> bootstrap(Async);

    struct BootstrapResult {
        asio::ip::udp::endpoint my_ep;
        asio::ip::udp::endpoint node_ep;
    };

    friend std::ostream& operator << (std::ostream&, const BootstrapResult&);

    std::expected<BootstrapResult, sys::error_code>
    bootstrap_single(bootstrap::Address, Async);

    std::vector<NodeContact> find_closest_nodes(
        NodeID target_id,
        Cancel&,
        asio::yield_context
    );

    std::string new_transaction_string();

    std::expected<void, sys::error_code> send_write_query(
        udp::endpoint destination,
        NodeID destination_id,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        Async
    );

    // http://bittorrent.org/beps/bep_0044.html#get-message
    boost::optional<BencodedMap> query_get_data(
        NodeID key,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dms,
        DebugCtx* dbg,
        Cancel&,
        asio::yield_context
    );

    boost::optional<BencodedMap> query_get_data2(
        NodeID key,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dead_man_switch,
        DebugCtx&,
        Cancel&,
        asio::yield_context
    );

    boost::optional<BencodedMap> query_get_data3(
        NodeID key,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dead_man_switch,
        DebugCtx&,
        Cancel&,
        asio::yield_context
    );


    struct TrackerNode {
        asio::ip::udp::endpoint node_endpoint;
        std::string announce_token;
    };

    std::expected<void, sys::error_code> tracker_do_search_peers(
        NodeID infohash,
        std::set<udp::endpoint>& peers,
        std::map<NodeID, TrackerNode>& responsible_nodes,
        Async
    );

    static bool closer_to(const NodeID& reference, const NodeID& left, const NodeID& right);

    template<class Evaluate>
    requires
        std::invocable<
            Evaluate,
            const Contact&,
            WatchDog&,
            util::AsyncQueue<NodeContact>&,
            Async
        >
    std::expected<void, sys::error_code>
    collect(
        DebugCtx&,
        const NodeID& target,
        Evaluate&&,
        Async
    );

    fs::path stored_contacts_path() const;

    void store_contacts() const;

    private:
    AsioExecutor _exec;
    ip::udp::endpoint _local_endpoint;
    std::unique_ptr<UdpMultiplexer> _multiplexer;
    NodeID _node_id;
    udp::endpoint _wan_endpoint;
    std::unique_ptr<RoutingTable> _routing_table;
    std::unique_ptr<Tracker> _tracker;
    std::unique_ptr<DataStore> _data_store;
    bool _ready;
    Cancel _cancel;

    struct ActiveRequest {
        udp::endpoint destination;
        std::function<void(BencodedMap&&)> callback;
    };
    uint32_t _next_transaction_id;

    // Reason for std::less<>:
    //   https://stackoverflow.com/questions/35525777/use-of-string-view-for-map-lookup
    std::map<std::string, ActiveRequest, std::less<>> _active_requests;

    std::vector<udp::endpoint> _bootstrap_endpoints;

    class Stats;
    std::unique_ptr<Stats> _stats;
    std::shared_ptr<dns::Resolver> _dns_resolver;
    uint32_t _mux_rx_limit;
    boost::filesystem::path _storage_dir;
    bootstrap::Config _bootstrap_config;
    metrics::DhtNode _metrics;
    util::LogPath _log_path;
};

} // namespace ouinet::bittorent
