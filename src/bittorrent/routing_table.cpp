#include "routing_table.h"

#include <set>

using namespace std;
using namespace ouinet::bittorrent::dht;

RoutingTable::RoutingTable(NodeID node_id) :
    _node_id(node_id)
{
    _root_node = std::make_unique<TreeNode>(NodeID::Range::max());
    _root_node->bucket = std::make_unique<RoutingBucket>();
}

void RoutingTable::TreeNode::split() {
    assert(bucket);
    assert(!left_child);
    assert(!right_child);

    /*
     * Buckets that may be split are never supposed to have candidates
     * in them.
     */
    // TODO: Should this hold true?
    //assert(bucket->verified_candidates.empty());
    //assert(bucket->unverified_candidates.empty());

    /*
     * Split the bucket.
     */
    left_child = std::make_unique<TreeNode>(range.reduce(0));
    left_child->bucket = std::make_unique<RoutingBucket>();

    right_child = std::make_unique<TreeNode>(range.reduce(1));
    right_child->bucket = std::make_unique<RoutingBucket>();

    for (const auto& node : bucket->nodes) {
        if (node.contact.id.bit(depth())) {
            right_child->bucket->nodes.push_back(node);
        } else {
            left_child->bucket->nodes.push_back(node);
        }
    }

    auto move_candidates = [&] ( deque<RoutingNode>& src
                               , deque<RoutingNode>& left
                               , deque<RoutingNode>& right ) {
        while (!src.empty()) {
            auto node = move(src.front());
            src.pop_front();

            if (node.contact.id.bit(depth())) { right.push_back(node); }
            else                              { left.push_back(node);  }
        }
    };

    move_candidates( bucket->verified_candidates
                   , left_child ->bucket->verified_candidates
                   , right_child->bucket->verified_candidates);

    move_candidates( bucket->unverified_candidates
                   , left_child ->bucket->unverified_candidates
                   , right_child->bucket->unverified_candidates);

    bucket = nullptr;
}

size_t RoutingTable::TreeNode::count_routing_nodes() const
{
    if (bucket) {
        return bucket->nodes.size();
    } else {
        return left_child ->count_routing_nodes()
             + right_child->count_routing_nodes();
    }
}

/*
 * Find the routing table bucket containing a particular ID in its namespace.
 *
 * If split_buckets is set, and the containing bucket does not have a routing
 * node in it for the ID, then try to split the bucket until there is room in
 * the resulting bucket for the ID. This may or may not succeed.
 *
 * No matter whether split_buckets is set, the returned bucket may or may not
 * contain a routing node for the target ID, and may or may not have room to
 * add such a node.
 */
RoutingBucket* RoutingTable::find_bucket(NodeID id, bool split_buckets)
{
    TreeNode* tree_node = _root_node.get();
    std::set<TreeNode*> ancestors;
    ancestors.insert(tree_node);
    bool node_contains_self = true;
    while (!tree_node->bucket) {
        if (id.bit(tree_node->depth())) {
            tree_node = tree_node->right_child.get();
        } else {
            tree_node = tree_node->left_child.get();
        }
        if (id.bit(tree_node->depth()) != _node_id.bit(tree_node->depth())) {
            node_contains_self = false;
        }
        ancestors.insert(tree_node);
    }

    if (split_buckets) {
        /*
         * If the contact is already in this bucket, return it.
         */
        for (size_t i = 0; i < tree_node->bucket->nodes.size(); i++) {
            if (tree_node->bucket->nodes[i].contact.id == id) {
                return tree_node->bucket.get();
            }
        }

        /*
         * If the bucket is full and allowed to be split, split it.
         *
         * A full bucket may be split in three conditions:
         * - if the bucket ID space contains _node_id;
         * - if the bucket depth is not a multiple of TREE_BASE (this turns the
         *   routing table into a 2^TREE_BASE-ary tree rather than a binary
         *   one, and saves in round trips);
         * - if the bucket is a descendent of the deepest ancestor of _node_id
         *   that contains at least BUCKET_SIZE nodes.
         */
        const int TREE_BASE = 5;
        TreeNode* exhaustive_root = exhaustive_routing_subtable_fragment_root();

        while (tree_node->bucket->nodes.size() == RoutingBucket::BUCKET_SIZE
                && tree_node->depth() < NodeID::bit_size) {
            if (
                !node_contains_self
                && (tree_node->depth() % TREE_BASE) == 0
                && !ancestors.count(exhaustive_root)
            ) {
                break;
            }

            tree_node->split();

            if (id.bit(tree_node->depth())) {
                tree_node = tree_node->right_child.get();
            } else {
                tree_node = tree_node->left_child.get();
            }

            if (_node_id.bit(tree_node->depth()) != id.bit(tree_node->depth())) {
                node_contains_self = false;
            }

            ancestors.insert(tree_node);

            // TODO: each bucket needs a refresh background coroutine.
        }
    }

    return tree_node->bucket.get();
}

/*
 * The routing table contains every known good node in the smallest subtree
 * that contains _node_id and has at least BUCKET_SIZE contacts in it.
 * This function computes the root of that subtree. Routing tree nodes
 * below this node may always be split when full.
 */
RoutingTable::TreeNode* RoutingTable::exhaustive_routing_subtable_fragment_root() const
{
    std::vector<TreeNode*> path;
    TreeNode* tree_node = _root_node.get();

    while (!tree_node->bucket) {
        path.push_back(tree_node);
        if (_node_id.bit(path.size())) {
            tree_node = tree_node->right_child.get();
        } else {
            tree_node = tree_node->left_child.get();
        }
    }

    size_t size = tree_node->bucket->nodes.size();

    while (size < RoutingBucket::BUCKET_SIZE && !path.empty()) {
        tree_node = path.back();
        path.pop_back();
        if (_node_id.bit(path.size())) {
            size += tree_node->left_child->count_routing_nodes();
        } else {
            size += tree_node->right_child->count_routing_nodes();
        }
    }

    return tree_node;
}

void RoutingTable::TreeNode::closest_routing_nodes( NodeID target
                                                  , size_t max_output
                                                  , std::vector<NodeContact>& output)
{
    if (output.size() >= max_output) {
        return;
    }
    if (bucket) {
        /*
         * Nodes are listed oldest first, so iterate in reverse order
         */
        for (auto it = bucket->nodes.rbegin(); it != bucket->nodes.rend(); ++it) {
            if (!it->is_bad()) {
                output.push_back(it->contact);
                if (output.size() >= max_output) {
                    break;
                }
            }
        }
    } else {
        if (target.bit(depth())) {
            right_child->closest_routing_nodes(target, max_output, output);
            left_child ->closest_routing_nodes(target, max_output, output);
        } else {
            left_child ->closest_routing_nodes(target, max_output, output);
            right_child->closest_routing_nodes(target, max_output, output);
        }
    }
}

/*
 * Find the $count nodes in the routing table, not known to be bad, that are
 * closest to $target.
 */
std::vector<NodeContact>
RoutingTable::find_closest_routing_nodes(NodeID target, size_t count)
{
    TreeNode* tree_node = _root_node.get();
    std::vector<TreeNode*> ancestors;
    ancestors.push_back(tree_node);

    while (!tree_node->bucket) {
        if (target.bit(tree_node->depth())) {
            tree_node = tree_node->right_child.get();
        }
        else {
            tree_node = tree_node->left_child.get();
        }

        ancestors.push_back(tree_node);
    }

    std::vector<NodeContact> output;

    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        (*it)->closest_routing_nodes(target, count, output);
        if (output.size() >= count) break;
    }

    return output;
}
