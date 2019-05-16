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

private:
    using Clock = std::chrono::steady_clock;

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

        void erase_candidate(const NodeContact&);
    };

public:
    RoutingTable(DhtNode&);
    RoutingTable(const RoutingTable&) = delete;

    std::vector<NodeContact> find_closest_routing_nodes(NodeID target, size_t count);

    void fail_node(NodeContact);

    void try_add_node(NodeContact, bool is_verified);

private:
    RoutingTable::Bucket* find_bucket(NodeID id);
    size_t bucket_id(const NodeID&) const;

private:
    DhtNode& _dht_node;
    NodeID _node_id;
    std::vector<Bucket> _buckets;
};

}}} // namespaces

