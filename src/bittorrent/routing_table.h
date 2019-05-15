#pragma once

#include <boost/asio/ip/udp.hpp>

#include <chrono>
#include <deque>

#include "node_contact.h"

namespace ouinet { namespace bittorrent { namespace dht {

class DhtNode;

class RoutingTable {
public:
    static const size_t BUCKET_SIZE = 8;
    using Clock = std::chrono::steady_clock;

private:
    struct RoutingNode {
        static inline constexpr std::chrono::minutes QUESTIONABLE_TIMEOUT() {
            return std::chrono::minutes(15);
        }
    
        NodeContact contact;
        Clock::time_point last_activity;
        int queries_failed;
        bool ping_ongoing;
    
        inline bool is_bad() const { return queries_failed > 3; }

        inline bool is_questionable() const {
            return last_activity + QUESTIONABLE_TIMEOUT() < Clock::now();
        }
    };

    struct Bucket {
        /*
         * Verified candidates have replied to a query.
         * Unverified candidates need to be pinged first.
         *
         * The number of nodes plus the number of candidates always stays below
         * BUCKET_SIZE.
         */
        std::vector<RoutingNode> nodes;
        std::deque<RoutingNode> verified_candidates;
        std::deque<RoutingNode> unverified_candidates;
    };

public:
    RoutingTable(NodeID);
    RoutingTable(const RoutingTable&) = delete;

    std::vector<NodeContact> find_closest_routing_nodes(NodeID target, size_t count);

    void fail_node(NodeContact, DhtNode&);

    void try_add_node(NodeContact, bool is_verified, DhtNode&);

private:
    RoutingTable::Bucket* find_bucket(NodeID id);
    size_t bucket_id(const NodeID&) const;

private:
    NodeID _node_id;
    std::vector<Bucket> _buckets;
};

}}} // namespaces

