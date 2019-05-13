#pragma once

#include <boost/asio/ip/udp.hpp>

#include <chrono>
#include <deque>

#include "node_contact.h"

namespace ouinet { namespace bittorrent { namespace dht {

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
    static const size_t BUCKET_SIZE = 8;

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

class DhtNode;

class RoutingTable {
    private:
    struct TreeNode {
        /*
         * A tree node is either a leaf with a bucket pointer,
         * or a non-leaf with children.
         */

        NodeID::Range range;

        TreeNode(NodeID::Range r) : range(std::move(r)) {}

        void split();
        size_t depth() const { return range.mask; }
        size_t count_routing_nodes() const;

        void closest_routing_nodes( const NodeID& target
                                  , size_t max_output
                                  , std::vector<NodeContact>& output);

        std::unique_ptr<TreeNode> left_child;
        std::unique_ptr<TreeNode> right_child;
        std::unique_ptr<RoutingBucket> bucket;
    };

    public:
    RoutingTable(NodeID);
    RoutingTable(const RoutingTable&) = delete;

    RoutingBucket* find_bucket(NodeID id, bool split_buckets);
    std::vector<NodeContact> find_closest_routing_nodes(NodeID target, size_t count);

    template<class F> void for_each_bucket(F&&);

    void routing_bucket_fail_node(RoutingBucket*, NodeContact, DhtNode&);

    private:
    TreeNode* exhaustive_routing_subtable_fragment_root() const;

    template<class F> void for_each_bucket(F&&, TreeNode*);

    private:
    NodeID _node_id;
    std::unique_ptr<TreeNode> _root_node;
};

template<class F>
void RoutingTable::for_each_bucket(F&& f) {
    for_each_bucket(std::forward<F>(f), _root_node.get());
}

template<class F>
void RoutingTable::for_each_bucket(F&& f, TreeNode* node) {
    if (node->bucket) {
        f(node->range, *node->bucket);
        return;
    }

    for_each_bucket(std::forward<F>(f), node->left_child.get());
    for_each_bucket(std::forward<F>(f), node->right_child.get());
}

}}} // namespaces

