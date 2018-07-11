#include "dht.h"
#include "udp_multiplexer.h"
#include "code.h"

#include "../or_throw.h"
#include "../util/condition_variable.h"
#include "../util/wait_condition.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <set>

#include <iostream>

namespace ouinet {
namespace bittorrent {

static
boost::asio::mutable_buffers_1 buffer(std::string& s) {
    return boost::asio::buffer(const_cast<char*>(s.data()), s.size());
}

std::string dht::NodeContact::to_string() const
{
    return id.to_hex() + " at " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}


dht::DhtNode::DhtNode(asio::io_service& ios, ip::address interface_address):
    _ios(ios),
    _interface_address(interface_address),
    _initialized(false),
    _tracker(std::make_unique<Tracker>(_ios))
{
}

void dht::DhtNode::start(sys::error_code& ec)
{
    udp::socket socket(_ios);

    if (_interface_address.is_v4()) {
        socket.open(udp::v4(), ec);
    } else {
        socket.open(udp::v6(), ec);
    }
    if (ec) {
        return;
    }

    udp::endpoint endpoint(_interface_address, 0);
    socket.bind(endpoint, ec);
    if (ec) return;

    _multiplexer = std::make_unique<UdpMultiplexer>(std::move(socket));

    _node_id = NodeID::zero();
    _next_transaction_id = 1;

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        receive_loop(yield);
    });

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        bootstrap(yield);
    });
}

void dht::DhtNode::tracker_get_peers(NodeID infohash, std::vector<tcp::endpoint>& peers, asio::yield_context yield)
{
    std::map<NodeID, TrackerNode> tracker_reply;
    tracker_search_peers(infohash, tracker_reply, yield);

    peers.clear();
    for (auto& i : tracker_reply) {
        peers.insert(peers.end(), i.second.peers.begin(), i.second.peers.end());
    }
}

void dht::DhtNode::tracker_announce(NodeID infohash, boost::optional<int> port, std::vector<tcp::endpoint>& peers, asio::yield_context yield)
{
    std::map<NodeID, TrackerNode> tracker_reply;
    tracker_search_peers(infohash, tracker_reply, yield);

    peers.clear();
    for (auto& i : tracker_reply) {
        peers.insert(peers.end(), i.second.peers.begin(), i.second.peers.end());
    }

    for (auto& i : tracker_reply) {
        /*
         * Fire-and-forget announce messages to each responsible node.
         */
        asio::spawn(_ios, [
            this,
            infohash,
            port,
            node_endpoint = i.second.node_endpoint,
            node_id = i.first,
            announce_token = i.second.announce_token
        ] (asio::yield_context yield) {
            BencodedMap announce_message;
            announce_message["id"] = _node_id.to_bytestring();
            announce_message["info_hash"] = infohash.to_bytestring();
            announce_message["token"] = announce_token;
            if (port) {
                announce_message["implied_port"] = int64_t(0);
                announce_message["port"] = int64_t(*port);
            } else {
                announce_message["implied_port"] = int64_t(1);
                announce_message["port"] = int64_t(0);
            }

            /*
             * Retry the announce message a couple of times.
             */
            const int TRIES = 5;
            for (int i = 0; i < TRIES; i++) {
                sys::error_code ec;

                BencodedMap announce_reply = send_query_await_reply(
                    node_endpoint,
                    node_id,
                    "announce_peer",
                    announce_message,
                    std::chrono::seconds(5),
                    yield[ec]
                );

                if (!ec) {
                    break;
                }
            }
        });
    }
}

void dht::DhtNode::receive_loop(asio::yield_context yield)
{
    while (true) {
        sys::error_code ec;

        /*
         * Later versions of boost::asio make it possible to (1) wait for a
         * datagram, (2) find out the size, (3) allocate a buffer, (4) recv
         * the datagram. Unfortunately, boost::asio 1.62 does not support that.
         */
        udp::endpoint sender;

        const boost::string_view packet
            = _multiplexer->receive(sender, yield[ec]);

        if (ec) break;

        // TODO: The bencode parser should only need a string_view.
        boost::optional<BencodedValue> decoded_message
            = bencoding_decode(packet.to_string());

        if (!decoded_message) {
            continue;
        }

        boost::optional<BencodedMap> message_map = decoded_message->as_map();
        if (!message_map) {
            continue;
        }

        if (!message_map->count("y") || !message_map->count("t")) {
            continue;
        }

        boost::optional<std::string> message_type = (*message_map)["y"].as_string();
        boost::optional<std::string> transaction_id = (*message_map)["t"].as_string();
        if (!message_type || !transaction_id) {
            continue;
        }

        if (*message_type == "q") {
            handle_query(sender, *message_map, yield);
        } else if (*message_type == "r" || *message_type == "e") {
            auto it = _active_requests.find(*transaction_id);
            if (it != _active_requests.end() && it->second.destination == sender) {
                it->second.callback(*message_map);
            }
        }
    }
}

std::string dht::DhtNode::new_transaction_string()
{
#if 0 // Useful for debugging
    std::stringstream ss;
    ss << _next_transaction_id++;
    return ss.str();
#else
    uint32_t transaction_id = _next_transaction_id++;

    if (transaction_id == 0) {
        return std::string(1 /* count */, '\0');
    }

    std::string ret;

    while (transaction_id) {
        unsigned char c = transaction_id & 0xff;
        transaction_id = transaction_id >> 8;
        ret += c;
    }

    return ret;
#endif
}

void dht::DhtNode::send_query( udp::endpoint destination
                             , std::string transaction
                             , std::string query_type
                             , BencodedMap query_arguments
                             , asio::yield_context yield)
{
    std::string message
        = bencoding_encode(BencodedMap { { "y", "q" }
                                       , { "q", std::move(query_type) }
                                       , { "a", std::move(query_arguments) }
                                       // TODO: version string
                                       , { "t", std::move(transaction) }
                                       });

    _multiplexer->send(buffer(message), destination, yield);
}

/*
 * Send a query message to a destination, and wait for either a reply, an error
 * reply, or a timeout.
 *
 * If destination_id is set, update the routing table in accordance with
 * whether a successful reply was received.
 */
BencodedMap dht::DhtNode::send_query_await_reply(
    udp::endpoint destination,
    boost::optional<NodeID> destination_id,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    asio::steady_timer::duration timeout,
    asio::yield_context yield
) {
    BencodedMap response; // Return value

    ConditionVariable reply_and_timeout_condition(_ios);
    boost::optional<sys::error_code> first_error_code;

    asio::steady_timer timeout_timer(_ios);
    timeout_timer.expires_from_now(timeout);
    timeout_timer.async_wait([&] (const sys::error_code&) {
        if (!first_error_code) {
            first_error_code = asio::error::timed_out;
        }
        reply_and_timeout_condition.notify();
    });

    std::string transaction = new_transaction_string();

    _active_requests[transaction]
        = { destination
          , [&] (const BencodedMap& response_) {
                if (first_error_code) return;
                first_error_code = sys::error_code(); // success;
                response = response_;
                timeout_timer.cancel();
            }
          };

    sys::error_code ec;

    send_query( destination
              , transaction
              , std::move(query_type)
              , std::move(query_arguments)
              , yield[ec]);

    if (ec) {
        first_error_code = ec;
        timeout_timer.cancel();
    }

    reply_and_timeout_condition.wait(yield);
    _active_requests.erase(transaction);

    if (destination_id) {
        NodeContact contact;
        contact.endpoint = destination;
        contact.id = *destination_id;
        if (*first_error_code || *response["y"].as_string() != "r") {
            /*
             * Record the failure in the routing table.
             */
            dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(*destination_id, false);
            routing_bucket_fail_node(routing_bucket, contact, yield);
        } else {
            /*
             * Add the node to the routing table, subject to space limitations.
             */
            dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(*destination_id, true);
            routing_bucket_try_add_node(routing_bucket, contact, true, yield);
        }
    }

    return or_throw(yield, *first_error_code, std::move(response));
}

void dht::DhtNode::handle_query( udp::endpoint sender
                               , BencodedMap query
                               , asio::yield_context yield)
{
    assert(query["y"].as_string() && *query["y"].as_string() == "q");

    boost::optional<std::string> transaction_ = query["t"].as_string();

    if (!transaction_) { return; }

    std::string transaction = *transaction_;

    auto send_error = [&] (int code, std::string description) {
        BencodedMap message;
        message["y"] = "e";
        message["t"] = transaction;
        message["e"] = BencodedList{code, description};
        std::string message_string = bencoding_encode(message);

        sys::error_code ec; // Ignored
        _multiplexer->send(buffer(message_string), sender, yield[ec]);
    };

    auto send_reply = [&] (BencodedMap reply) {
        reply["id"] = _node_id.to_bytestring();

        BencodedMap message;
        message["y"] = "r";
        message["t"] = transaction;
        message["r"] = reply;
        std::string message_string = bencoding_encode(message);

        sys::error_code ec; // Ignored
        _multiplexer->send(buffer(message_string), sender, yield[ec]);
    };

    if (!query["q"].is_string()) {
        send_error(203, "Missing field 'q'");
        return;
    }
    std::string query_type = *query["q"].as_string();

    if (!query["a"].is_map()) {
        send_error(203, "Missing field 'a'");
        return;
    }
    BencodedMap arguments = *query["a"].as_map();

    boost::optional<std::string> sender_id = arguments["id"].as_string();
    if (!sender_id) {
        send_error(203, "Missing argument 'id'");
        return;
    }
    if (sender_id->size() != 20) {
        send_error(203, "Malformed argument 'id'");
        return;
    }
    NodeContact contact;
    contact.id = NodeID::from_bytestring(*sender_id);
    contact.endpoint = sender;

    /*
     * Per BEP 43, if the query contains a read-only flag, do not consider the
     * sender for any routing purposes.
     */
    boost::optional<int64_t> read_only_flag = arguments["ro"].as_int();
    if (!read_only_flag || *read_only_flag != 1) {
        /*
        * Add the sender to the routing table.
        */
        dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(contact.id, true);
        routing_bucket_try_add_node(routing_bucket, contact, false, yield);
    }

    if (query_type == "ping") {
        BencodedMap reply;
        send_reply(reply);
        return;
    } else if (query_type == "find_node") {
        boost::optional<std::string> target_id_ = arguments["target"].as_string();
        if (!target_id_) {
            send_error(203, "Missing argument 'target'");
            return;
        }
        if (target_id_->size() != 20) {
            send_error(203, "Malformed argument 'target'");
            return;
        }
        NodeID target_id = NodeID::from_bytestring(*target_id_);

        BencodedMap reply;

        std::vector<dht::NodeContact> contacts
            = _routing_table->find_closest_routing_nodes(target_id, RoutingBucket::BUCKET_SIZE);
        std::string nodes;
        if (!contacts.empty() && contacts[0].id == target_id) {
            nodes += contacts[0].id.to_bytestring();
            nodes += encode_endpoint(contacts[0].endpoint);
        } else {
            for (auto& contact : contacts) {
                nodes += contact.id.to_bytestring();
                nodes += encode_endpoint(contact.endpoint);
            }
        }
        if (_interface_address.is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        send_reply(reply);
        return;
    } else if (query_type == "get_peers") {
        boost::optional<std::string> infohash_ = arguments["info_hash"].as_string();
        if (!infohash_) {
            send_error(203, "Missing argument 'info_hash'");
            return;
        }
        if (infohash_->size() != 20) {
            send_error(203, "Malformed argument 'info_hash'");
            return;
        }
        NodeID infohash = NodeID::from_bytestring(*infohash_);

        BencodedMap reply;

        std::vector<dht::NodeContact> contacts
            = _routing_table->find_closest_routing_nodes(infohash, RoutingBucket::BUCKET_SIZE);
        std::string nodes;
        for (auto& contact : contacts) {
            nodes += contact.id.to_bytestring();
            nodes += encode_endpoint(contact.endpoint);
        }
        if (_interface_address.is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        /*
         * 50 peers will comfortably fit in a single UDP packet even in the
         * worst case.
         */
        const int NUM_PEERS = 50;
        std::vector<tcp::endpoint> peers = _tracker->list_peers(infohash, NUM_PEERS);
        if (!peers.empty()) {
            BencodedList peer_list;
            for (auto& peer : peers) {
                peer_list.push_back(encode_endpoint(peer));
            }
            reply["values"] = peer_list;
        }

        send_reply(reply);
        return;
    } else if (query_type == "announce_peer") {
        boost::optional<std::string> infohash_ = arguments["info_hash"].as_string();
        if (!infohash_) {
            send_error(203, "Missing argument 'info_hash'");
            return;
        }
        if (infohash_->size() != 20) {
            send_error(203, "Malformed argument 'info_hash'");
            return;
        }
        NodeID infohash = NodeID::from_bytestring(*infohash_);

        boost::optional<std::string> token_ = arguments["token"].as_string();
        if (!token_) {
            send_error(203, "Missing argument 'token'");
            return;
        }
        std::string token = *token_;
        boost::optional<int64_t> port_ = arguments["port"].as_int();
        if (!port_) {
            send_error(203, "Missing argument 'port'");
            return;
        }
        boost::optional<int64_t> implied_port_ = arguments["implied_port"].as_int();
        int effective_port;
        if (implied_port_ && *implied_port_ == 1) {
            effective_port = sender.port();
        } else {
            effective_port = *port_;
        }

        /*
         * Reject announce_peer requests for which there are more than enough
         * better responsible known nodes.
         *
         * TODO: This can be done in a more efficient way once the routing
         * table code stabilizes.
         */
        {
            bool contains_self = false;
            std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(infohash, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
            for (auto& i : closer_nodes) {
                if (closer_to(infohash, _node_id, i.id)) {
                    contains_self = true;
                }
            }
            if (!contains_self) {
                send_error(201, "This torrent is not my responsibility");
                return;
            }
        }

        if (!_tracker->verify_token(sender.address(), token)) {
            send_error(203, "Incorrect announce token");
            return;
        }

        _tracker->add_peer(infohash, tcp::endpoint(sender.address(), effective_port));

        BencodedMap reply;
        send_reply(reply);
        return;
    } else {
        send_error(204, "Query type not implemented");
        return;
    }
}


static
asio::ip::udp::endpoint resolve( asio::io_context& ioc
                               , const std::string& addr
                               , const std::string& port
                               , asio::yield_context yield)
{
    using asio::ip::udp;

    sys::error_code ec;

    udp::resolver::query bootstrap_query(addr, port);
    udp::resolver bootstrap_resolver(ioc);
    udp::resolver::iterator it = bootstrap_resolver.async_resolve(bootstrap_query, yield[ec]);

    if (ec) return or_throw<udp::endpoint>(yield, ec);

    while (it != udp::resolver::iterator()) {
        return it->endpoint();
    }

    return or_throw<udp::endpoint>(yield, asio::error::not_found);
}

void dht::DhtNode::bootstrap(asio::yield_context yield)
{
    sys::error_code ec;

    // Other servers include router.utorrent.com:6881 and dht.transmissionbt.com:6881
    auto bootstrap_ep = resolve(_ios, "router.bittorrent.com", "6881", yield[ec]);

    if (ec) {
        std::cout << "Unable to resolve bootstrap server, giving up\n";
        return;
    }

    BencodedMap initial_ping_message;
    initial_ping_message["id"] = _node_id.to_bytestring();

    BencodedMap initial_ping_reply = send_query_await_reply(
        bootstrap_ep,
        boost::none,
        "ping",
        initial_ping_message,
        std::chrono::seconds(15),
        yield[ec]
    );
    if (ec) {
        std::cout << "Bootstrap server does not reply, giving up\n";
        return;
    }

    boost::optional<std::string> my_ip = initial_ping_reply["ip"].as_string();
    if (!my_ip) {
        std::cout << "Unexpected bootstrap server reply, giving up\n";
        return;
    }
    boost::optional<asio::ip::udp::endpoint> my_endpoint = decode_endpoint(*my_ip);
    if (!my_endpoint) {
        std::cout << "Unexpected bootstrap server reply, giving up\n";
        return;
    }

    _node_id = NodeID::generate(my_endpoint->address());
    _routing_table = std::make_unique<RoutingTable>(_node_id);

    /*
     * TODO: Make bootstrap node handling and ID determination more reliable.
     *
     * Ideally, this process should start a coroutine that continuously tries
     * to keep a list of hardcoded bootstrap servers up to date, resolving and
     * pinging them; and the find_node procedure can then use these endpoints
     * as additional start points for any node searches.
     *
     * There also needs to be vastly more retrying and fallbacks here.
     */

    std::vector<udp::endpoint> bootstrap_endpoints;
    bootstrap_endpoints.push_back(bootstrap_ep);
    /*
     * Lookup our own ID, constructing a basic path to ourselves.
     */
    find_closest_nodes(_node_id, bootstrap_endpoints, yield);

    /*
     * For each bucket in the routing table, lookup a random ID in that range.
     * This ensures that every node that should route to us, knows about us.
     */
    refresh_routing_table(yield);

    _initialized = true;
}


void dht::DhtNode::refresh_routing_table(asio::yield_context yield)
{
    WaitCondition wc(_ios);

    _routing_table->for_each_bucket(
        [&] (const NodeID::Range& range, RoutingBucket& bucket) {
            spawn(_ios, [this, range, lock = wc.lock()]
                        (asio::yield_context yield) {
                            find_closest_nodes(range.random_id(), {}, yield);
                        });
        });

    wc.wait(yield);
}

std::vector<dht::NodeContact> dht::DhtNode::find_closest_nodes(
    NodeID target_id,
    std::vector<udp::endpoint> extra_starting_points,
    asio::yield_context yield
) {
    auto query = [this, target_id] (
        udp::endpoint node_endpoint,
        boost::optional<NodeID> node_id,
        std::vector<NodeContact>& closer_nodes,
        std::vector<NodeContact>& closer_nodes6,
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
    ) -> bool {
        return query_find_node(
            target_id,
            node_endpoint,
            node_id,
            closer_nodes,
            closer_nodes6,
            yield
        );
    };

    return search_dht_for_nodes(target_id, 8, query, extra_starting_points, yield);
}

std::vector<dht::NodeContact> dht::DhtNode::search_dht_for_nodes(
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
     *     of closest good nodes seen so far.
     *
     * @return True if this node is eligible for inclusion in the output
     *     set of the search. False otherwise.
     */
    std::function<bool(
        udp::endpoint node_endpoint,
        boost::optional<NodeID> node_id,
        std::vector<NodeContact>& closer_nodes,
        std::vector<NodeContact>& closer_nodes6,
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
) {
    struct Candidate {
        udp::endpoint endpoint;
        bool confirmed_good;
        bool in_progress;
    };
    auto order_ids = [&target_id] (const NodeID& left, const NodeID& right) {
        return closer_to(target_id, left, right);
    };
    std::map<NodeID, Candidate, decltype(order_ids)> candidates(order_ids);
    int confirmed_nodes = 0;
    int in_progress_endpoints = 0;

    for (auto& contact : _routing_table->find_closest_routing_nodes(target_id, max_nodes)) {
        Candidate candidate;
        candidate.endpoint = contact.endpoint;
        candidate.confirmed_good = false;
        candidate.in_progress = false;
        candidates[contact.id] = candidate;
    }

    const int THREADS = 64;
    WaitCondition all_done(_ios);
    ConditionVariable candidate_available(_ios);
    for (int thread = 0; thread < THREADS; thread++) {
        asio::spawn(_ios, [&, lock = all_done.lock()] (asio::yield_context yield) {
            while (true) {
                boost::optional<NodeID> candidate_id = boost::none;
                boost::optional<udp::endpoint> endpoint = boost::none;

                /*
                 * Try the closest untried candidate...
                 */
                for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                    if (it->second.confirmed_good || it->second.in_progress) {
                        continue;
                    }
                    candidate_id = it->first;
                    endpoint = it->second.endpoint;
                    it->second.in_progress = true;
                    break;
                }

                /*
                 * or, failing that, try one of the bootstrap nodes.
                 */
                if (!endpoint) {
                    if (!extra_starting_points.empty()) {
                        endpoint = extra_starting_points.back();
                        extra_starting_points.pop_back();
                    }
                }

                if (!endpoint) {
                    if (in_progress_endpoints == 0) {
                        break;
                    }
                    candidate_available.wait(yield);
                    continue;
                }
                in_progress_endpoints++;

                sys::error_code ec;
                std::vector<NodeContact> result_nodes;
                std::vector<NodeContact> result_nodes6;
                std::function<void(boost::optional<NodeContact> displaced_node, asio::yield_context yield)> on_promote;

                bool accepted = query_node(*endpoint, candidate_id, result_nodes, result_nodes6, on_promote, yield[ec]);

                in_progress_endpoints--;
                candidate_available.notify();

                if (ec) {
                    if (candidate_id) {
                        candidates.erase(*candidate_id);
                    }
                    continue;
                }

                if (candidate_id && candidates.count(*candidate_id) > 0) {
                    if (!accepted) {
                        candidates.erase(*candidate_id);
                    } else {
                        candidates[*candidate_id].confirmed_good = true;
                        candidates[*candidate_id].in_progress = false;
                        confirmed_nodes++;

                        boost::optional<NodeContact> displaced_node;

                        if (confirmed_nodes >= max_nodes) {
                            /*
                             * Remove remote candidates until there are $max_nodes confirmed candidates
                             * left, and no nonconfirmed candidates more remote than the most remote
                             * confirmed candidate remain.
                             */
                            while (true) {
                                auto it = candidates.rbegin();
                                assert(it != candidates.rend());
                                if (it->second.confirmed_good) {
                                    if (confirmed_nodes == max_nodes) {
                                        break;
                                    } else {
                                        confirmed_nodes--;

                                        assert(!displaced_node);

                                        displaced_node = NodeContact {
                                            .id = it->first,
                                            .endpoint = it->second.endpoint
                                        };
                                    }
                                }
                                candidates.erase(it->first);
                            }
                        }

                        if (on_promote) {
                            on_promote(displaced_node, yield);
                        }
                    }
                }

                auto& contacts = _interface_address.is_v4()
                               ? result_nodes
                               : result_nodes6;

                /*
                 * Only add new nodes after promoting the candidate. This saves
                 * remote contacts from being added and then immediately purged.
                 */
                bool added = false;
                for (const NodeContact& contact : contacts) {
                    if (confirmed_nodes >= max_nodes) {
                        if (closer_to(target_id, candidates.rbegin()->first, contact.id)) {
                            continue;
                        }
                    }

                    if (candidates.count(contact.id) > 0) {
                        continue;
                    }

                    candidates[contact.id] = Candidate {
                        .endpoint       = contact.endpoint,
                        .confirmed_good = false,
                        .in_progress    = false
                    };

                    added = true;
                }

                if (added) {
                    candidate_available.notify();
                }
            }
        });
    }

    all_done.wait(yield);

    std::vector<NodeContact> output;
    for (auto it : candidates) {
        assert(it.second.confirmed_good);
        NodeContact contact;
        contact.id = it.first;
        contact.endpoint = it.second.endpoint;
        output.push_back(contact);
    }
    return output;
}


void dht::DhtNode::send_ping(NodeContact contact, asio::yield_context yield)
{
    sys::error_code ec;

    send_query( contact.endpoint
              , new_transaction_string()
              , "ping"
              , BencodedMap{ { "id", _node_id.to_bytestring() } }
              , yield[ec]);
}

/**
 * Send a find_node query to a target node, and parse the reply.
 * @return True when received a valid response, false otherwise.
 */
bool dht::DhtNode::query_find_node(
    NodeID target_id,
    udp::endpoint node_endpoint,
    boost::optional<NodeID> node_id,
    std::vector<NodeContact>& closer_nodes,
    std::vector<NodeContact>& closer_nodes6,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap find_node_message;
    find_node_message["id"] = _node_id.to_bytestring();
    find_node_message["target"] = target_id.to_bytestring();

    BencodedMap find_node_reply = send_query_await_reply(
        node_endpoint,
        node_id,
        "find_node",
        find_node_message,
        std::chrono::seconds(2),
        yield[ec]
    );

    if (ec) {
        return false;
    }

    if (!find_node_reply["y"].as_string() || *find_node_reply["y"].as_string() != "r") {
        return false;
    }

    boost::optional<BencodedMap> arguments = find_node_reply["r"].as_map();
    if (!arguments) {
        return false;
    }

    bool nodes_present = true;

    boost::optional<std::string> nodes = (*arguments)["nodes"].as_string();

    if (nodes) {
        if (!decode_contacts_v4(*nodes, closer_nodes))
            nodes_present = false;
    } else if (_interface_address.is_v4()) {
        // This field is required in v4 requests and optional elsewhere
        nodes_present = false;
    }

    boost::optional<std::string> nodes6 = (*arguments)["nodes6"].as_string();

    if (nodes) {
        if (!decode_contacts_v6(*nodes6, closer_nodes6))
            nodes_present = false;
    } else if (_interface_address.is_v6()) {
        // This field is required in v6 requests and optional elsewhere
        nodes_present = false;
    }

    return nodes_present;
}

/**
 * Perform a get_peers search. Returns the peers found, as well as necessary
 * data to later perform an announce operation.
 */
void dht::DhtNode::tracker_search_peers(
    NodeID infohash,
    std::map<NodeID, TrackerNode>& tracker_reply,
    asio::yield_context yield
) {
    /*
     * Contains the peers reported by the closest N nodes found so far.
     */
    tracker_reply.clear();

    auto query = [this, infohash, &tracker_reply] (
        udp::endpoint node_endpoint,
        boost::optional<NodeID> node_id,
        std::vector<NodeContact>& closer_nodes,
        std::vector<NodeContact>& closer_nodes6,
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
    ) -> bool {
        sys::error_code ec;

        BencodedMap get_peers_reply = send_query_await_reply(
            node_endpoint,
            node_id,
            "get_peers",
            BencodedMap { { "id", _node_id.to_bytestring() }
                        , { "info_hash", infohash.to_bytestring() } },
            std::chrono::seconds(2),
            yield[ec]
        );

        if (ec) {
            return or_throw(yield, ec, false);
        }

        if (!get_peers_reply["y"].as_string() || *get_peers_reply["y"].as_string() != "r") {
            return false;
        }

        boost::optional<BencodedMap> get_peers_arguments = get_peers_reply["r"].as_map();
        if (!get_peers_arguments) {
            return false;
        }

        bool got_peers = false;
        boost::optional<BencodedList> encoded_peers = (*get_peers_arguments)["values"].as_list();
        boost::optional<std::string> announce_token = (*get_peers_arguments)["token"].as_string();
        if (encoded_peers && announce_token) {
            TrackerNode tracker_node;
            tracker_node.node_endpoint = node_endpoint;
            tracker_node.announce_token = *announce_token;

            got_peers = true;
            for (auto& peer : *encoded_peers) {
                boost::optional<std::string> peer_string = peer.as_string();
                if (peer_string) {
                    boost::optional<udp::endpoint> endpoint = decode_endpoint(*peer_string);
                    if (endpoint) {
                        tcp::endpoint tcp_endpoint(endpoint->address(), endpoint->port());
                        tracker_node.peers.push_back(tcp_endpoint);
                    }
                }
            }

            on_promote = [node_id, tracker_node = std::move(tracker_node), &tracker_reply] (
                boost::optional<NodeContact> displaced_node,
                asio::yield_context yield
            ) {
                assert(node_id);
                if (displaced_node) {
                    tracker_reply.erase(displaced_node->id);
                }
                tracker_reply[*node_id] = std::move(tracker_node);
            };
        }

        bool got_nodes = true;
        boost::optional<std::string> nodes = (*get_peers_arguments)["nodes"].as_string();
        if (nodes) {
            if (!decode_contacts_v4(*nodes, closer_nodes))
                got_nodes = false;
        } else if (_interface_address.is_v4()) {
            // This field is required in v4 requests and optional elsewhere
            got_nodes = false;
        }

        boost::optional<std::string> nodes6 = (*get_peers_arguments)["nodes6"].as_string();
        if (nodes) {
            if (!decode_contacts_v6(*nodes6, closer_nodes6))
                got_nodes = false;
        } else if (_interface_address.is_v6()) {
            // This field is required in v6 requests and optional elsewhere
            got_nodes = false;
        }

        /*
         * A reply that contains peers may or may not also contain nodes.
         * If it does not, follow up the get_peers message with a find_node.
         * Ignore errors; a reply that contains peers but no nodes,
         * even after a find_node, is still valid.
         */
        if (!got_nodes) {
            query_find_node(
                infohash,
                node_endpoint,
                node_id,
                closer_nodes,
                closer_nodes6,
                yield
            );
        }

        return got_peers;
    };

    search_dht_for_nodes(infohash, RESPONSIBLE_TRACKERS_PER_SWARM, query, {}, yield);
}



/*
 * Record a node in the routing table, space permitting. If there is no space,
 * check for node replacement opportunities. If is_verified is not set, ping
 * the target contact before adding it.
 */
void dht::DhtNode::routing_bucket_try_add_node( RoutingBucket* bucket
                                              , NodeContact contact
                                              , bool is_verified
                                              , asio::yield_context yield)
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
            send_ping(contact, yield);
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
                send_ping(contact, yield);
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
                send_ping(bucket->nodes[i].contact, yield);
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

/*
 * Record a failure of a routing table node to respond to a query. If this
 * makes the node bad, try to replace it with a queued candidate.
 */
void dht::DhtNode::routing_bucket_fail_node( RoutingBucket* bucket
                                           , NodeContact contact
                                           , asio::yield_context yield)
{
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
            send_ping(contact, yield);
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
        send_ping(contact, yield);
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

bool dht::DhtNode::closer_to(const NodeID& reference, const NodeID& left, const NodeID& right)
{
    for (unsigned int i = 0; i < sizeof(reference.buffer); i++) {
        unsigned char l = left.buffer[i] ^ reference.buffer[i];
        unsigned char r = right.buffer[i] ^ reference.buffer[i];
        if (l < r) {
            return true;
        }
        if (r < l) {
            return false;
        }
    }
    return false;
}

MainlineDht::MainlineDht(asio::io_service& ios):
    _ios(ios)
{
}

MainlineDht::~MainlineDht()
{
}

void MainlineDht::set_interfaces(const std::vector<asio::ip::address>& addresses)
{
    std::set<asio::ip::address> addresses_used;

    for (asio::ip::address address : addresses) {
        addresses_used.insert(address);

        if (!_nodes.count(address)) {
            sys::error_code ec;
            std::unique_ptr<dht::DhtNode> node = std::make_unique<dht::DhtNode>(_ios, address);
            node->start(ec);
            if (!ec) {
                _nodes[address] = std::move(node);
            }
        }
    }

    for (auto it = _nodes.begin(); it != _nodes.end(); ) {
        if (addresses_used.count(it->first)) {
            ++it;
        } else {
            it = _nodes.erase(it);
        }
    }
}


} // bittorrent namespace
} // ouinet namespace
