#pragma once

#include <boost/asio/ip/udp.hpp>

#include <chrono>
#include <deque>
#include <set>

#include "node_contact.h"

namespace ouinet { namespace bittorrent { namespace dht {

class DhtNode;

class RoutingTable {
public:
    static constexpr size_t BUCKET_SIZE = 8;

private:
    using Clock = std::chrono::steady_clock;
    using SendPing = std::function<void(const NodeContact&)>;

    struct RoutingNode {
        NodeContact contact;

        Clock::time_point recv_time;  // time of last message received
        Clock::time_point reply_time; // time of last reply received

        int queries_failed;
        bool ping_ongoing;
    
        inline bool is_good() const {
            using namespace std::chrono_literals;

            auto now = Clock::now();

            return queries_failed <= 2
                && recv_time  >= now - 15min
                && reply_time >= now - 2h;
        }

        // "questionable" is defined in BEP0005
        // http://www.bittorrent.org/beps/bep_0005.html#routing-table
        inline bool is_questionable() const {
            using namespace std::chrono_literals;
            return recv_time < Clock::now() - 15min;
        }
    };

    struct Bucket {
        /*
         * Verified candidates have replied to a query.
         * Unverified candidates need to be pinged first.
         *
         * The number of nodes plus the number of candidates always stays below
         * BUCKET_SIZE + questionable_count_in(nodes).
         */
        std::vector<RoutingNode> nodes;
        std::deque<RoutingNode> verified_candidates;
        std::deque<RoutingNode> unverified_candidates;
    };

public:
    RoutingTable(const NodeID& node_id, SendPing);
    RoutingTable(const RoutingTable&) = delete;

    std::vector<NodeContact> find_closest_routing_nodes(NodeID target, size_t count);

    void fail_node(NodeContact);

    void try_add_node(NodeContact, bool is_verified);

    NodeID max_distance(size_t bucket_id) const;

    NodeID node_id() const { return _node_id; }

    std::set<NodeContact> dump_contacts() const;

private:
    RoutingTable::Bucket* find_bucket(NodeID id);
    size_t find_bucket_id(const NodeID&) const;
    bool would_split_bucket(size_t bucket_id, const NodeID& new_id) const;
    void split_bucket(size_t bucket_id);

private:
    NodeID _node_id;
    SendPing _send_ping;
    std::vector<Bucket> _buckets;
};

}}} // namespaces

