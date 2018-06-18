#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <deque>
#include <vector>

#include "bencoding.h"
#include "../namespaces.h"
#include "../util/signal.h"
#include "../util/wait_condition.h"

namespace ouinet {
namespace bittorrent {

struct NodeID {
    std::array<unsigned char, 20> buffer;

    bool bit(int n) const;
    std::string to_hex() const;
    std::string to_bytestring() const;
    static NodeID from_bytestring(const std::string& bytestring);
    static NodeID zero();

    inline bool operator==(const NodeID& other) const { return buffer == other.buffer; }
};

namespace dht {

namespace ip = asio::ip;
using ip::tcp;
using ip::udp;

struct NodeContact {
    NodeID id;
    udp::endpoint endpoint;

    std::string to_string() const;
    inline bool operator==(const NodeContact& other) const { return id == other.id && endpoint == other.endpoint; }
};

struct RoutingNode {
    static inline constexpr std::chrono::minutes QUESTIONABLE_TIMEOUT() { return std::chrono::minutes(15); }

    NodeContact contact;
    std::chrono::steady_clock::time_point last_activity;
    int queries_failed;
    bool questionable_ping_ongoing;

    inline bool is_bad() const { return queries_failed > 3; }
    inline bool is_questionable() const { return last_activity + QUESTIONABLE_TIMEOUT() < std::chrono::steady_clock::now(); }
};

struct RoutingBucket {
    static const int BUCKET_SIZE = 8;

    std::vector<RoutingNode> nodes;
    std::deque<RoutingNode> verified_candidates;
    std::deque<RoutingNode> unverified_candidates;
    /*
     * Verified candidates have replied to a query.
     * Unverified candidates need to be pinged first.
     *
     * The number of nodes plus the number of candidates always stays below 
     * BUCKET_SIZE.
     */
};

struct RoutingTreeNode {
    /*
     * A tree node is either a leaf with a bucket pointer,
     * or a non-leaf with children.
     */

    std::unique_ptr<RoutingTreeNode> left_child;
    std::unique_ptr<RoutingTreeNode> right_child;
    std::unique_ptr<RoutingBucket> bucket;
};

class DhtNode {
    public:
    DhtNode(asio::io_service& ios, ip::address interface_address);
    void start(sys::error_code& ec);
    bool initialized() const { return _initialized; }

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
    void refresh_tree_node(dht::RoutingTreeNode* node, NodeID id, int depth, WaitCondition& refresh_done);
    std::vector<NodeContact> find_closest_nodes(
        NodeID id,
        std::vector<udp::endpoint> extra_starting_points,
        asio::yield_context yield
    );

    void send_ping(NodeContact contact);

    RoutingBucket* find_routing_bucket(NodeID id, bool split_buckets);
    void routing_bucket_try_add_node(RoutingBucket* bucket, NodeContact contact, bool verify_contact);
    void routing_bucket_fail_node(RoutingBucket* bucket, NodeContact contact);
    RoutingTreeNode* exhaustive_routing_subtable_fragment_root() const;
    std::vector<NodeContact> find_closest_routing_nodes(NodeID target, unsigned int count);

    void choose_id(ip::address address);
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
    std::unique_ptr<RoutingTreeNode> _routing_table;

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
