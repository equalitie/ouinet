#include "routing_table.h"
#include "dht.h"
#include "proximity_map.h"

#include <set>
#include <iostream>

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;

//--------------------------------------------------------------------
template<class From, class To, class Pred>
static void move_elements(From& from, To&& to, const Pred& predicate)
{
    for (size_t i = 0; i < from.size();) {
        if (predicate(from[i])) {
            to.push_back(move(from[i]));
            from.erase(from.begin() + i);
        } else {
            ++i;
        }
    }
}

template<class R, class P>
static void erase_if(R& r, P&& p)
{
    r.erase( std::remove_if(std::begin(r), std::end(r), std::forward<P>(p))
           , std::end(r));
}

template<class Q>
static void erase_front_questionables(Q& q)
{
    while (!q.empty() && q[0].is_questionable()) {
        q.pop_front();
    }
}

//--------------------------------------------------------------------

RoutingTable::RoutingTable(const NodeID& node_id, SendPing send_ping)
    : _node_id(node_id)
    , _send_ping(move(send_ping))
    , _buckets(1)
{
}

//  max_distance(0)   -> 111..111
//  max_distance(1)   -> 011..111
//  max_distance(2)   -> 001..111
//  ...
//  max_distance(159) -> 000..000
NodeID RoutingTable::max_distance(size_t bucket_id) const
{
    NodeID ret = NodeID::max();

    for (size_t i = 0; i < bucket_id; ++i) {
        ret.set_bit(i, false);
    }

    return ret;
}

bool RoutingTable::would_split_bucket( size_t bucket_id
                                     , const NodeID& new_id) const
{
    auto dst = new_id ^ _node_id;

    if (dst > max_distance(bucket_id)) {
        return false;
    }

    auto& b = _buckets[bucket_id];

    if (b.nodes.size() < BUCKET_SIZE) return false;

    auto half_dst = max_distance(bucket_id + 1);

    size_t cnt = 0;
    if (dst <= half_dst) { cnt++; }

    for (auto& n : b.nodes) {
        if ((n.contact.id ^ _node_id) <= half_dst) {
            cnt++;
        }
    }

    // We can only split if after splitting we wont end up with all the old
    // nodes together wit the new_id being in only one of the new buckets.
    return 0 < cnt && cnt <= BUCKET_SIZE;
}

size_t RoutingTable::find_bucket_id(const NodeID& id) const
{
    NodeID distance = _node_id ^ id;
    NodeID max = NodeID::max();

    size_t ret = 0;

    for (size_t i = 0; i < _buckets.size(); ++i) {
        if (distance > max) {
            return ret;
        }
        max.set_bit(i, false);
        ret = i;
    }

    return ret;
}

RoutingTable::Bucket* RoutingTable::find_bucket(NodeID id)
{
    return &_buckets[find_bucket_id(id)];
}

void RoutingTable::split_bucket(size_t i)
{
    assert(i == _buckets.size() - 1);

    Bucket new_bucket;

    auto new_bucket_max_size = max_distance(i+1);

    auto belongs_to_new_bucket = [&] (const RoutingNode& n) {
        return (n.contact.id ^ _node_id) <= new_bucket_max_size;
    };

    move_elements(_buckets[i].nodes
                 , new_bucket.nodes
                 , belongs_to_new_bucket);

    move_elements(_buckets[i].verified_candidates
                 , new_bucket.verified_candidates
                 , belongs_to_new_bucket);

    move_elements(_buckets[i].unverified_candidates
                 , new_bucket.unverified_candidates
                 , belongs_to_new_bucket);

    _buckets.push_back(move(new_bucket));
}

std::vector<NodeContact>
RoutingTable::find_closest_routing_nodes(NodeID target, size_t count)
{
    std::vector<NodeContact> output;

    if (count == 0) return output;

    size_t bucket_i = find_bucket_id(target);
    bool done = false;

    for (size_t i = bucket_i; i < _buckets.size() && !done; ++i) {
        for (auto& n : _buckets[i].nodes) {
            output.push_back(n.contact);
            if (output.size() >= count) done = true;
            if (done) break;
        }
    }

    while (bucket_i && !done) {
        --bucket_i;
        for (auto& n : _buckets[bucket_i].nodes) {
            output.push_back(n.contact);
            if (output.size() >= count) done = true;
            if (done) break;
        }
    }

    return output;
}

/*
 * Record a node in the routing table, space permitting. If there is no space,
 * check for node replacement opportunities. If is_verified is not set, ping
 * the target contact before adding it.
 */
void RoutingTable::try_add_node(NodeContact contact, bool is_verified)
{
    size_t bucket_id = find_bucket_id(contact.id);
    Bucket* bucket = &_buckets[bucket_id];

    auto now = Clock::now();

    /*
     * Check whether the contact is already in the routing table. If so, bump it.
     */
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].contact == contact) {
            RoutingNode node = bucket->nodes[i];

            node.recv_time = now;

            if (is_verified) {
                node.reply_time     = now;
                node.queries_failed = 0;
                node.ping_ongoing   = false;
            }

            bucket->nodes.erase(bucket->nodes.begin() + i);
            bucket->nodes.push_back(node);
            return;
        }
    }

    erase_if(bucket->verified_candidates,   [&] (auto& c) { return c.contact == contact; });
    erase_if(bucket->unverified_candidates, [&] (auto& c) { return c.contact == contact; });

    /*
     * If there is space in the bucket, add the node. If it is unverified,
     * ping it instead; on success, the node will be added.
     */
    if (bucket->nodes.size() < BUCKET_SIZE) {
        if (is_verified) {
            bucket->nodes.push_back(RoutingNode {
                .contact        = contact,
                .recv_time      = now,
                .reply_time     = now,
                .queries_failed = 0,
                .ping_ongoing   = false,
            });
        } else {
            _send_ping(contact);
        }
        return;
    }

    if (would_split_bucket(bucket_id, contact.id)) {
        if (is_verified) {

            bucket->nodes.push_back(RoutingNode {
                .contact        = contact,
                .recv_time      = now,
                .reply_time     = now,
                .queries_failed = 0,
                .ping_ongoing   = false,
            });

            split_bucket(bucket_id);

            assert(_buckets.size() == bucket_id + 2);
            assert(_buckets[bucket_id].nodes.size() <= BUCKET_SIZE);
            assert(_buckets[bucket_id+1].nodes.size() <= BUCKET_SIZE);
        } else {
            _send_ping(contact);
        }
        return;
    }

    /*
     * Check whether there are any bad nodes in the table. If so, replace it,
     * per above.
     */
    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (!bucket->nodes[i].is_good()) {
            if (is_verified) {
                bucket->nodes.erase(bucket->nodes.begin() + i);

                bucket->nodes.push_back(RoutingNode {
                    .contact        = contact,
                    .recv_time      = now,
                    .reply_time     = now,
                    .queries_failed = 0,
                    .ping_ongoing   = false,
                });
            } else {
                _send_ping(contact);
            }
            return;
        }
    }

    /*
     * Count the number of questionable nodes, and make sure they are all being
     * pinged to check whether they are still good.
     */
    size_t questionable_nodes = 0;

    for (auto& n : bucket->nodes) {
        if (n.is_questionable()) {
            questionable_nodes++;

            if (!n.ping_ongoing) {
                _send_ping(n.contact);
                n.ping_ongoing = true;
            }
        }
    }

    /*
     * Add the contact as a candidate.
     */
    RoutingNode candidate {
        .contact        = contact,
        .recv_time      = now,
        .reply_time     = is_verified ? now : Clock::time_point(),
        .queries_failed = 0,
        .ping_ongoing   = false
    };

    if (is_verified) {
        if (questionable_nodes > 0) {
            bucket->verified_candidates.push_back(candidate);
        }
    } else {
        /*
         * An unverified contact can either replace other unverified contacts,
         * or verified contacts that have become questionable (read: old).
         */
        erase_front_questionables(bucket->verified_candidates);

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

/*
 * Record a failure of a routing table node to respond to a query. If this
 * makes the node bad, try to replace it with a queued candidate.
 */
void RoutingTable::fail_node(NodeContact contact)
{
    Bucket* bucket = find_bucket(contact.id);

    /*
     * Find the contact in the routing table.
     */
    size_t node_i = bucket->nodes.size();

    for (size_t i = 0; i < bucket->nodes.size(); i++) {
        if (bucket->nodes[i].contact == contact) {
            node_i = i;
            break;
        }
    }

    if (node_i == bucket->nodes.size()) return;

    bucket->nodes[node_i].queries_failed++;

    if (bucket->nodes[node_i].is_good()) {
        if (bucket->nodes[node_i].is_questionable()) {
            bucket->nodes[node_i].ping_ongoing = true;
            _send_ping(contact);
        }
        return;
    }

    /*
     * The node is bad. Try to replace it with one of the queued replacements.
     */
    erase_front_questionables(bucket->verified_candidates);
    erase_front_questionables(bucket->unverified_candidates);

    if (!bucket->verified_candidates.empty()) {
        /*
         * If there is a verified candidate available, use it.
         */
        bucket->nodes.erase(bucket->nodes.begin() + node_i);

        auto c = bucket->verified_candidates[0];
        bucket->verified_candidates.pop_front();

        RoutingNode node {
            .contact        = c.contact,
            .recv_time      = c.recv_time,
            .reply_time     = c.reply_time,
            .queries_failed = 0,
            .ping_ongoing   = false
        };

        for (size_t i = 0; i < bucket->nodes.size(); i++) {
            if (bucket->nodes[i].recv_time > node.recv_time) {
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
        _send_ping(contact);
    }

    /*
     * Cleanup superfluous candidates.
     */
    size_t questionable_nodes = 0;

    for (auto& n : bucket->nodes) {
        if (n.is_questionable()) questionable_nodes++;
    }

    while (bucket->verified_candidates.size() > questionable_nodes) {
        bucket->verified_candidates.pop_front();
    }

    while (bucket->verified_candidates.size() + bucket->unverified_candidates.size() > questionable_nodes) {
        bucket->unverified_candidates.pop_front();
    }
}

set<NodeContact> RoutingTable::dump_contacts() const
{
    using asio::ip::udp;

    set<NodeContact> ret;

    for (auto& bucket : _buckets) {
        for (auto& node : bucket.nodes) {
            ret.insert(node.contact);
        }
        for (auto& node : bucket.verified_candidates) {
            ret.insert(node.contact);
        }
    }

    return ret;
}
