#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <vector>

#include "bencoding.h"
#include "dht_tracker.h"
#include "node_id.h"
#include "routing_table.h"

#include "../namespaces.h"
#include "../util/signal.h"
#include "../util/wait_condition.h"

namespace ouinet {
namespace bittorrent {

class UdpMultiplexer;

namespace dht {

namespace ip = asio::ip;
using ip::tcp;
using ip::udp;


class DhtNode {
    public:
    const size_t RESPONSIBLE_TRACKERS_PER_SWARM = 8;

    public:
    DhtNode(asio::io_service& ios, ip::address interface_address);
    void start(asio::yield_context);
    bool initialized() const { return _initialized; }

    /**
     * Query peers for a bittorrent swarm surrounding a particular infohash.
     * This returns a randomized subset of all such peers, not the entire swarm.
     */
    void tracker_get_peers(NodeID infohash, std::vector<tcp::endpoint>& peers, asio::yield_context yield);

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
    void tracker_announce(NodeID infohash, boost::optional<int> port, std::vector<tcp::endpoint>& peers, asio::yield_context yield);

    private:
    void receive_loop(asio::yield_context yield);

    void send_query( udp::endpoint destination
                   , std::string transaction
                   , std::string query_type
                   , BencodedMap query_arguments
                   , asio::yield_context yield);

    BencodedMap send_query_await_reply(
        udp::endpoint destination,
        boost::optional<NodeID> destination_id,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        asio::steady_timer::duration timeout,
        asio::yield_context yield
    );

    void handle_query(udp::endpoint sender, BencodedMap query, asio::yield_context);

    void bootstrap(asio::yield_context yield);

    void refresh_routing_table(asio::yield_context yield);

    std::vector<NodeContact> find_closest_nodes(
        NodeID target_id,
        asio::yield_context yield
    );

    std::string new_transaction_string();

    // http://bittorrent.org/beps/bep_0005.html#ping
    void send_ping(NodeContact contact);

    struct TrackerNode {
        udp::endpoint node_endpoint;
        std::vector<tcp::endpoint> peers;
        std::string announce_token;
    };

    // http://bittorrent.org/beps/bep_0005.html#find-node
    bool query_find_node(
        NodeID target_id,
        udp::endpoint node_endpoint,
        boost::optional<NodeID> node_id,
        std::vector<NodeContact>& closer_nodes,
        std::vector<NodeContact>& closer_nodes6,
        asio::yield_context
    );

    // http://bittorrent.org/beps/bep_0005.html#get-peers
    boost::optional<TrackerNode>
    query_get_peers( NodeID infohash
                   , udp::endpoint node_endpoint
                   , boost::optional<NodeID> node_id
                   , std::vector<NodeContact>& closer_nodes
                   , std::vector<NodeContact>& closer_nodes6
                   , asio::yield_context);

    std::map<NodeID, TrackerNode>
    tracker_search_peers(NodeID infohash, asio::yield_context);

    void routing_bucket_try_add_node( RoutingBucket*
                                    , NodeContact
                                    , bool is_verified);

    void routing_bucket_fail_node(RoutingBucket*, NodeContact);

    static bool closer_to(const NodeID& reference, const NodeID& left, const NodeID& right);

    template<class Evaluate>
    void collect(const NodeID& target, size_t max_nodes, Evaluate&&, asio::yield_context) const;

    private:
    asio::io_service& _ios;
    ip::address _interface_address;
    std::unique_ptr<UdpMultiplexer> _multiplexer;
    NodeID _node_id;
    bool _initialized;
    std::unique_ptr<RoutingTable> _routing_table;
    std::unique_ptr<Tracker> _tracker;

    struct ActiveRequest {
        udp::endpoint destination;
        std::function<void(const BencodedMap&)> callback;
    };
    uint32_t _next_transaction_id;
    std::map<std::string, ActiveRequest> _active_requests;

    std::vector<udp::endpoint> _bootstrap_endpoints;
};

} // dht namespace

class MainlineDht {
    public:
    MainlineDht(asio::io_service& ios);
    ~MainlineDht();

    void set_interfaces(const std::vector<asio::ip::address>& addresses, asio::yield_context);
    std::vector<asio::ip::tcp::endpoint> find_peers(NodeID torrent_id, Signal<void()>& cancel, asio::yield_context yield);
    void announce(NodeID torrent_id, Signal<void()>& cancel, asio::yield_context yield);

    private:
    asio::io_service& _ios;
    std::map<asio::ip::address, std::unique_ptr<dht::DhtNode>> _nodes;
};

} // bittorrent namespace
} // ouinet namespace
