#include "routing_table.h"
#include "dht.h"

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

void RoutingTable::TreeNode::closest_routing_nodes( const NodeID& target
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

/*
 * Record a failure of a routing table node to respond to a query. If this
 * makes the node bad, try to replace it with a queued candidate.
 */
void RoutingTable::routing_bucket_fail_node( RoutingBucket* bucket
                                           , NodeContact contact
                                           , DhtNode& dht_node)
{
    sys::error_code ec;
    /*
     * Find the contact in the routing table.
     */
    size_t node_index;
    bool found = false;
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].contact == contact) {
            node_index = i;
            found = true;
        }
    }
    if (!found) {
        return;
    }

    bucket->nodes[node_index].queries_failed++;
    if (!bucket->nodes[node_index].is_bad()) {
        if (bucket->nodes[node_index].is_questionable()) {
            bucket->nodes[node_index].questionable_ping_ongoing = true;
            dht_node.send_ping(contact);
        }
        return;
    }

    /*
     * The node is bad. Try to replace it with one of the queued replacements.
     */
    /*
     * Get rid of outdated candidates.
     */
    while (!bucket->verified_candidates.empty() && bucket->verified_candidates[0].is_questionable()) {
        bucket->verified_candidates.pop_front();
    }
    while (!bucket->unverified_candidates.empty() && bucket->unverified_candidates[0].is_questionable()) {
        bucket->unverified_candidates.pop_front();
    }

    if (!bucket->verified_candidates.empty()) {
        /*
         * If there is a verified candidate available, use it.
         */
        bucket->nodes.erase(bucket->nodes.begin() + node_index);

        RoutingNode node;
        node.contact = bucket->verified_candidates[0].contact;
        node.last_activity = bucket->verified_candidates[0].last_activity;
        node.queries_failed = 0;
        node.questionable_ping_ongoing = false;
        bucket->verified_candidates.pop_front();

        for (size_t i = 0; i < bucket->nodes.size(); i++) {
            if (bucket->nodes[i].last_activity > node.last_activity) {
                bucket->nodes.insert(bucket->nodes.begin() + i, node);
                break;
            }
        }
    } else if (!bucket->unverified_candidates.empty()) {
        /*
         * If there is an unverified candidate available, ping it. The reply
         * handler will replace the bad node.
         */
        NodeContact contact = bucket->unverified_candidates[0].contact;
        bucket->unverified_candidates.pop_front();
        dht_node.send_ping(contact);
        if (ec) return;
    }

    /*
     * Cleanup superfluous candidates.
     */
    size_t questionable_nodes = 0;
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].is_questionable()) {
            questionable_nodes++;
        }
    }
    while (bucket->verified_candidates.size() > questionable_nodes) {
        bucket->verified_candidates.pop_front();
    }
    while (bucket->verified_candidates.size() + bucket->unverified_candidates.size() > questionable_nodes) {
        bucket->unverified_candidates.pop_front();
    }
}


/*
 * Record a node in the routing table, space permitting. If there is no space,
 * check for node replacement opportunities. If is_verified is not set, ping
 * the target contact before adding it.
 */
void RoutingTable::routing_bucket_try_add_node( RoutingBucket* bucket
                                              , NodeContact contact
                                              , bool is_verified
                                              , DhtNode& dht_node)
{
    /*
     * Check whether the contact is already in the routing table. If so, bump it.
     */
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].contact == contact) {
            RoutingNode node = bucket->nodes[i];
            node.last_activity = std::chrono::steady_clock::now();
            if (is_verified) {
                node.queries_failed = 0;
                node.questionable_ping_ongoing = false;
            }
            bucket->nodes.erase(bucket->nodes.begin() + i);
            bucket->nodes.push_back(node);
            return;
        }
    }

    /*
     * Remove the contact from the candidate table, if necessary.
     */
    for (size_t i = 0; i < bucket->verified_candidates.size(); i++) {
        if (bucket->verified_candidates[i].contact == contact) {
            bucket->verified_candidates.erase(bucket->verified_candidates.begin() + i);
            break;
        }
    }
    for (size_t i = 0; i < bucket->unverified_candidates.size(); i++) {
        if (bucket->unverified_candidates[i].contact == contact) {
            bucket->unverified_candidates.erase(bucket->unverified_candidates.begin() + i);
            break;
        }
    }
    /*
     * If we get here, the contact is neither in the routing table nor in the
     * candidate table.
     */

    /*
     * If there is space in the bucket, add the node. If it is unverified,
     * ping it instead; on success, the node will be added.
     */
    if (bucket->nodes.size() < RoutingBucket::BUCKET_SIZE) {
        if (is_verified) {
            RoutingNode node;
            node.contact = contact;
            node.last_activity = std::chrono::steady_clock::now();
            node.queries_failed = 0;
            node.questionable_ping_ongoing = false;
            bucket->nodes.push_back(node);
        } else {
            dht_node.send_ping(contact);
        }
        return;
    }

    /*
     * Check whether there are any bad nodes in the table. If so, replace it,
     * per above.
     */
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].is_bad()) {
            if (is_verified) {
                bucket->nodes.erase(bucket->nodes.begin() + i);

                RoutingNode node;
                node.contact = contact;
                node.last_activity = std::chrono::steady_clock::now();
                node.queries_failed = 0;
                node.questionable_ping_ongoing = false;
                bucket->nodes.push_back(node);
            } else {
                dht_node.send_ping(contact);
            }
            return;
        }
    }

    /*
     * Count the number of questionable nodes, and make sure they are all being
     * pinged to check whether they are still good.
     */
    size_t questionable_nodes = 0;
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].is_questionable()) {
            questionable_nodes++;
            if (!bucket->nodes[i].questionable_ping_ongoing) {
                dht_node.send_ping(bucket->nodes[i].contact);
                bucket->nodes[i].questionable_ping_ongoing = true;
            }
        }
    }

    /*
     * Add the contact as a candidate.
     */
    RoutingNode candidate;
    candidate.contact = contact;
    candidate.last_activity = std::chrono::steady_clock::now();
    /*
     * Other fields are meaningless for candidates.
     */

    if (is_verified) {
        if (questionable_nodes > 0) {
            bucket->verified_candidates.push_back(candidate);
        }
    } else {
        /*
         * An unverified contact can either replace other unverified contacts,
         * or verified contacts that have become questionable (read: old).
         */
        while (!bucket->verified_candidates.empty() && bucket->verified_candidates[0].is_questionable()) {
            bucket->verified_candidates.pop_front();
        }
        if (bucket->verified_candidates.size() < questionable_nodes) {
            bucket->unverified_candidates.push_back(candidate);
        }
    }
    while (bucket->verified_candidates.size() > questionable_nodes) {
        bucket->verified_candidates.pop_front();
    }
    while (bucket->verified_candidates.size() + bucket->unverified_candidates.size() > questionable_nodes) {
        bucket->unverified_candidates.pop_front();
    }
}



