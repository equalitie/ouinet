#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <vector>

#include "bencoding.h"
#include "node_id.h"
#include "routing_table.h"
#include "../namespaces.h"
#include "../util/signal.h"
#include "../util/wait_condition.h"

namespace ouinet {
namespace bittorrent {
namespace dht {

namespace ip = asio::ip;
using ip::tcp;
using ip::udp;


class DhtNode {
    public:
    DhtNode(asio::io_service& ios, ip::address interface_address);
    void start(sys::error_code& ec);
    bool initialized() const { return _initialized; }

    void tracker_get_peers(NodeID infohash, std::vector<tcp::endpoint>& peers, asio::yield_context yield);

    private:
    void receive_loop(asio::yield_context yield);
    void send_query_await_reply(
        udp::endpoint destination,
        boost::optional<NodeID> destination_id,
        const std::string& query_type,
        const BencodedMap& query_arguments,
        BencodedMap& response,
        asio::steady_timer::duration timeout,
        asio::yield_context yield
    );
    void handle_query(udp::endpoint sender, BencodedMap query);

    void bootstrap(asio::yield_context yield);
    void refresh_routing_table(asio::yield_context yield);
    std::vector<NodeContact> find_closest_nodes(
        NodeID target_id,
        std::vector<udp::endpoint> extra_starting_points,
        asio::yield_context yield
    );


    std::vector<NodeContact> search_dht_for_nodes(
        NodeID target_id,
        int max_nodes,
        /**
         * Called to query a particular node for nodes closer to the search
         * target, as well as any payload query for the search.
         *
         * @param node_endpoint Endpoint of the node to query.
         * @param node_id Node ID of the node to query, if any.
         * @param closer_nodes If the query returns any nodes found, this field
         *     is to store the ipv4 nodes, in find_node "nodes" encoding.
         * @param closer_nodes6 If the query returns any nodes found, this field
         *     is to store the ipv6 nodes, in find_node "nodes6" encoding.
         * @param on_promote Called if the queried node becomes part of the set
         *     of closest good nodes seen so far. This can only happen if
         *     query_node returns true.
         *
         * @return True if this node is eligible for inclusion in the output
         *     set of the search. False otherwise.
         */
        std::function<bool(
            udp::endpoint node_endpoint,
            boost::optional<NodeID> node_id,
            std::string& closer_nodes,
            std::string& closer_nodes6,
            /**
             * Called if the queried node becomes part of the set of closest
             * good nodes seen so far. Only ever invoked if query_node()
             * returned true, and node_id is not empty.
             *
             * @param displaced_node The node that is removed from the closest
             *     set to make room for the queried node, if any.
             */
            std::function<void(
                boost::optional<NodeContact> displaced_node,
                asio::yield_context yield
            )>& on_promote,
            asio::yield_context yield
        )> query_node,
        std::vector<udp::endpoint> extra_starting_points,
        asio::yield_context yield
    );

    void send_ping(NodeContact contact);
    bool query_find_node(
        NodeID target_id,
        udp::endpoint node_endpoint,
        boost::optional<NodeID> node_id,
        std::string& closer_nodes,
        std::string& closer_nodes6,
        asio::yield_context yield
    );

    void routing_bucket_try_add_node(RoutingBucket* bucket, NodeContact contact, bool is_verified);
    void routing_bucket_fail_node(RoutingBucket* bucket, NodeContact contact);

    static bool closer_to(const NodeID& reference, const NodeID& left, const NodeID& right);
    static std::string encode_endpoint(udp::endpoint endpoint);
    static boost::optional<udp::endpoint> decode_endpoint(std::string endpoint);

    private:
    asio::io_service& _ios;
    ip::address _interface_address;
    uint16_t _port;
    udp::socket _socket;
    NodeID _node_id;
    bool _initialized;
    std::unique_ptr<RoutingTable> _routing_table;

    struct ActiveRequest {
        udp::endpoint destination;
        Signal<void(const BencodedMap&)>* callback;
    };
    uint32_t _next_transaction_id;
    std::map<std::string, ActiveRequest> _active_requests;
    std::string _rx_buffer;
};

} // dht namespace

class MainlineDht {
    public:
    MainlineDht(asio::io_service& ios);
    ~MainlineDht();

    void set_interfaces(const std::vector<asio::ip::address>& addresses);
    std::vector<asio::ip::tcp::endpoint> find_peers(NodeID torrent_id, Signal<void()>& cancel, asio::yield_context yield);
    void announce(NodeID torrent_id, Signal<void()>& cancel, asio::yield_context yield);

    private:
    asio::io_service& _ios;
    std::map<asio::ip::address, std::unique_ptr<dht::DhtNode>> _nodes;
};

} // bittorrent namespace
} // ouinet namespace
