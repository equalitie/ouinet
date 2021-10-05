#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/filesystem/path.hpp>

#include <chrono>
#include <vector>
#include <set>

#include <asio_utp/udp_multiplexer.hpp>

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
#include "../util/async_queue.h"
#include "../util/watch_dog.h"

namespace ouinet {
namespace bittorrent {

class UdpMultiplexer;

asio::ip::udp::endpoint resolve(
    const asio::executor&,
    asio::ip::udp ipv,
    const std::string& addr,
    const std::string& port,
    Signal<void()>& cancel_signal,
    asio::yield_context yield
);

namespace ip = asio::ip;
using ip::udp;

class DebugCtx;

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
    DhtNode( const asio::executor&
           , boost::filesystem::path storage_dir = boost::filesystem::path());

    DhtNode( asio::io_context& ctx
           , boost::filesystem::path storage_dir = boost::filesystem::path())
        : DhtNode(ctx.get_executor(), std::move(storage_dir))
    {}

    void start(udp::endpoint, asio::yield_context yield);
    void start(asio_utp::udp_multiplexer, asio::yield_context yield);
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
    std::set<udp::endpoint> tracker_get_peers(
        NodeID infohash,
        Cancel&,
        asio::yield_context
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
    std::set<udp::endpoint> tracker_announce(
        NodeID infohash,
        boost::optional<int> port,
        Cancel&,
        asio::yield_context
    );

    /**
     * Search the DHT for BEP-44 immutable data item with key $key.
     * @return The data stored in the DHT under $key, or boost::none if no such
     *         data was found.
     */
    boost::optional<BencodedValue> data_get_immutable(
        const NodeID& key,
        Cancel&,
        asio::yield_context
    );

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
        const util::Ed25519PublicKey& public_key,
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
    BencodedMap send_ping(
        NodeContact contact,
        Cancel&,
        asio::yield_context
    );

    void send_ping(NodeContact contact);

    // http://bittorrent.org/beps/bep_0005.html#find-node
    bool query_find_node(
        NodeID target_id,
        Contact node,
        std::vector<NodeContact>& closer_nodes,
        Cancel&,
        asio::yield_context
    );

    bool query_find_node2(
        NodeID target_id,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dead_man_switch,
        DebugCtx* dbg,
        Cancel&,
        asio::yield_context
    );

    // http://bittorrent.org/beps/bep_0005.html#get-peers
    boost::optional<BencodedMap> query_get_peers(
        NodeID infohash,
        Contact node,
        util::AsyncQueue<NodeContact>& closer_nodes,
        WatchDog& dms,
        DebugCtx* dbg,
        Cancel&,
        asio::yield_context
    );

    bool is_v4() const { return _local_endpoint.address().is_v4(); }
    bool is_v6() const { return _local_endpoint.address().is_v6(); }

    udp::endpoint local_endpoint() const { return _local_endpoint; }
    udp::endpoint wan_endpoint() const { return _wan_endpoint; }

    ~DhtNode();

    asio::executor get_executor() { return _exec; }

    NodeID node_id() const { return _node_id; }

    private:
    void receive_loop(asio::yield_context);
    void store_contacts_loop(asio::yield_context);

    void send_datagram(
        udp::endpoint destination,
        const BencodedMap& query_arguments
    );
    void send_datagram(
        udp::endpoint destination,
        const BencodedMap& query_arguments,
        Cancel&,
        asio::yield_context
    );

    void send_query(
        udp::endpoint destination,
        std::string transaction,
        std::string query_type,
        BencodedMap query_arguments,
        Cancel&,
        asio::yield_context
    );

    BencodedMap send_query_await_reply(
        Contact,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        WatchDog* dms,
        DebugCtx* dbg,
        Cancel&,
        asio::yield_context
    );

    void handle_query(udp::endpoint sender, BencodedMap& query);

    void bootstrap(asio::yield_context);

    struct BootstrapResult {
        asio::ip::udp::endpoint my_ep;
        asio::ip::udp::endpoint node_ep;
    };

    using Address = boost::variant< udp::endpoint
                                  , std::string /* domain name */>;

    BootstrapResult
    bootstrap_single(Address, Cancel, asio::yield_context);

    std::vector<NodeContact> find_closest_nodes(
        NodeID target_id,
        Cancel&,
        asio::yield_context
    );

    std::string new_transaction_string();

    void send_write_query(
        udp::endpoint destination,
        NodeID destination_id,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        Cancel&,
        asio::yield_context
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
    void tracker_do_search_peers(
        NodeID infohash,
        std::set<udp::endpoint>& peers,
        std::map<NodeID, TrackerNode>& responsible_nodes,
        Cancel&,
        asio::yield_context
    );

    static bool closer_to(const NodeID& reference, const NodeID& left, const NodeID& right);

    template<class Evaluate>
    void collect(
        DebugCtx&,
        const NodeID& target,
        Evaluate&&,
        Cancel,
        asio::yield_context
    );

    fs::path stored_contacts_path() const;

    void store_contacts() const;

    static
    std::set<NodeContact>
    read_stored_contacts(const asio::executor&, boost::filesystem::path, Cancel, asio::yield_context);

    private:
    asio::executor _exec;
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
    boost::filesystem::path _storage_dir;
};

} // dht namespace

class MainlineDht {
    public:
    MainlineDht( const asio::executor&
               , boost::filesystem::path storage_dir = boost::filesystem::path());

    MainlineDht( asio::io_context& ctx
               , boost::filesystem::path storage_dir = boost::filesystem::path())
        : MainlineDht(ctx.get_executor(), std::move(storage_dir))
    {}

    MainlineDht(const MainlineDht&) = delete;
    MainlineDht& operator=(const MainlineDht&) = delete;

    ~MainlineDht();

    void set_endpoints(const std::set<udp::endpoint>&);
    void set_endpoint(asio_utp::udp_multiplexer);
    udp::endpoint set_endpoint(asio_utp::udp_multiplexer, asio::yield_context);


    std::set<udp::endpoint> local_endpoints() const {
        std::set<udp::endpoint> ret;
        for (auto& p : _nodes) { ret.insert(p.first); }
        return ret;
    }

    std::set<udp::endpoint> wan_endpoints() const {
        std::set<udp::endpoint> ret;
        for (auto& p : _nodes) { ret.insert(p.second->wan_endpoint()); }
        return ret;
    }

    /*
     * TODO: announce() and put() functions don't have any real error detection.
     */
    std::set<udp::endpoint> tracker_announce(
        NodeID infohash,
        boost::optional<int> port,
        Cancel,
        asio::yield_context
    );

    void mutable_put(const MutableDataItem&, Cancel&, asio::yield_context);

    std::set<udp::endpoint> tracker_get_peers(NodeID infohash, Cancel&, asio::yield_context);
    std::set<udp::endpoint> tracker_get_peers(NodeID infohash, asio::yield_context yield)
        { Cancel cancel; return tracker_get_peers(infohash, cancel, yield); }
    boost::optional<BencodedValue> immutable_get(NodeID key, Cancel&, asio::yield_context);
    boost::optional<BencodedValue> immutable_get(NodeID key, asio::yield_context yield)
        { Cancel cancel; return immutable_get(key, cancel, yield); }
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
        Cancel&,
        asio::yield_context
    );
    boost::optional<MutableDataItem> mutable_get(
        const util::Ed25519PublicKey& public_key,
        boost::string_view salt,
        asio::yield_context yield
    )
    { Cancel cancel; return mutable_get(public_key, salt, cancel, yield); }

    asio::executor get_executor() { return _exec; }

    bool all_ready() const {
        for (const auto& n : _nodes) {
            if (!n.second->ready()) return false;
        }
        return true;
    }
    void wait_all_ready(Cancel&, asio::yield_context);

    void stop() {
        _nodes.clear();
    }

    private:
    asio::executor _exec;
    std::map<udp::endpoint, std::unique_ptr<dht::DhtNode>> _nodes;
    Cancel _cancel;
    boost::filesystem::path _storage_dir;
};

} // bittorrent namespace
} // ouinet namespace
