#include "dht.h"

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
    _socket(ios),
    _initialized(false),
    _rx_buffer(65536, '\0')
{
}

void dht::DhtNode::start(sys::error_code& ec)
{
    if (_interface_address.is_v4()) {
        _socket.open(udp::v4(), ec);
    } else {
        _socket.open(udp::v6(), ec);
    }
    if (ec) {
        return;
    }

    udp::endpoint endpoint(_interface_address, 0);
    _socket.bind(endpoint, ec);
    if (ec) {
        return;
    }
    _port = _socket.local_endpoint().port();

    _node_id = NodeID::zero();
    _next_transaction_id = 1;

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        receive_loop(yield);
    });

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        bootstrap(yield);
    });
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
        std::size_t size = _socket.async_receive_from(buffer(_rx_buffer), sender, yield[ec]);
        if (ec) {
            break;
        }

        boost::optional<BencodedValue> decoded_message
            = bencoding_decode(_rx_buffer.substr(0, size));

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
            handle_query(sender, *message_map);
        } else if (*message_type == "r" || *message_type == "e") {
            auto it = _active_requests.find(*transaction_id);
            if (it != _active_requests.end() && it->second.destination == sender) {
                (*it->second.callback)(*message_map);
            }
        }
    }
}

/*
 * Send a query message to a destination, and wait for either a reply, an error
 * reply, or a timeout.
 *
 * If destination_id is set, update the routing table in accordance with
 * whether a successful reply was received.
 */
void dht::DhtNode::send_query_await_reply(
    udp::endpoint destination,
    boost::optional<NodeID> destination_id,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    BencodedMap& response,
    asio::steady_timer::duration timeout,
    asio::yield_context yield
) {
    std::string transaction_string;
    uint32_t transaction_id = _next_transaction_id++;
    while (transaction_id) {
        unsigned char c = transaction_id & 0xff;
        transaction_id = transaction_id >> 8;
        transaction_string += c;
    }

    BencodedMap message;
    message["y"] = "q";
    message["q"] = query_type;
    message["a"] = query_arguments;
    // TODO: version string
    message["t"] = transaction_string;
    std::string message_string = bencoding_encode(message);

    ConditionVariable reply_or_timeout_condition(_ios);
    bool received_response;

    Signal<void(const BencodedMap&)> callback;
    auto connection = callback.connect([&] (const BencodedMap& response_) {
        received_response = true;
        response = response_;
        reply_or_timeout_condition.notify();
    });
    ActiveRequest request_descriptor;
    request_descriptor.destination = destination;
    request_descriptor.callback = &callback;
    _active_requests[transaction_string] = request_descriptor;

    asio::steady_timer timeout_timer(_ios);
    asio::spawn(_ios, [&] (asio::yield_context yield) {
        sys::error_code ec;
        timeout_timer.expires_from_now(timeout);
        timeout_timer.async_wait(yield[ec]);
        if (!ec) {
            received_response = false;
            reply_or_timeout_condition.notify();
        }
    });

    sys::error_code ec;
    _socket.async_send_to(asio::buffer(message_string.data(), message_string.size()), destination, yield[ec]);
    /*
     * Ignore errors. If the message isn't sent properly, so be it.
     */

    reply_or_timeout_condition.wait(yield);
    _active_requests.erase(transaction_string);
    timeout_timer.cancel();

    if (destination_id) {
        NodeContact contact;
        contact.endpoint = destination;
        contact.id = *destination_id;
        if (!received_response || *response["y"].as_string() != "r") {
            /*
             * Record the failure in the routing table.
             */
            dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(*destination_id, false);
            routing_bucket_fail_node(routing_bucket, contact);
        } else {
            /*
             * Add the node to the routing table, subject to space limitations.
             */
            dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(*destination_id, true);
            routing_bucket_try_add_node(routing_bucket, contact, true);
        }
    }

    if (!received_response) {
         or_throw(yield, asio::error::timed_out);
    }
}

void dht::DhtNode::handle_query(udp::endpoint sender, BencodedMap query)
{
    assert(query["y"].as_string() && *query["y"].as_string() == "q");

    boost::optional<std::string> transaction_ = query["t"].as_string();
    if (!transaction_) {
        return;
    }
    std::string transaction = *transaction_;
    auto send_error = [this, sender, transaction] (int code, std::string description) {
        asio::spawn(_ios, [this, sender, transaction, code, description] (asio::yield_context yield) {
            BencodedList arguments;
            arguments.push_back(code);
            arguments.push_back(description);

            BencodedMap message;
            message["y"] = "e";
            message["t"] = transaction;
            message["e"] = arguments;
            std::string message_string = bencoding_encode(message);

            /*
             * Ignore errors.
             */
            sys::error_code ec;
            _socket.async_send_to(asio::buffer(message_string.data(), message_string.size()), sender, yield[ec]);
        });
    };
    auto send_reply = [this, sender, transaction, node_id = _node_id] (BencodedMap reply) {
        asio::spawn(_ios, [this, sender, transaction, &reply, node_id] (asio::yield_context yield) {
            reply["id"] = node_id.to_bytestring();

            BencodedMap message;
            message["y"] = "r";
            message["t"] = transaction;
            message["r"] = reply;
            std::string message_string = bencoding_encode(message);

            /*
             * Ignore errors.
             */
            sys::error_code ec;
            _socket.async_send_to(asio::buffer(message_string.data(), message_string.size()), sender, yield[ec]);
        });
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
        routing_bucket_try_add_node(routing_bucket, contact, false);
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

        std::vector<dht::NodeContact> contacts
            = _routing_table->find_closest_dht_nodes(target_id, RoutingBucket::BUCKET_SIZE);

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
        BencodedMap reply;
        if (_interface_address.is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }
        send_reply(reply);
        return;
//    } else if (query_type == "get_peers") {
//    } else if (query_type == "announce_peer") {
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

    BencodedMap initial_ping_reply;
    send_query_await_reply(
        bootstrap_ep,
        boost::none,
        "ping",
        initial_ping_message,
        initial_ping_reply,
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
    NodeID id,
    std::vector<udp::endpoint> extra_starting_points,
    asio::yield_context yield
) {
    struct Candidate {
        udp::endpoint endpoint;
        bool confirmed_good;
        bool in_progress;
    };
    auto order_ids = [&id] (const NodeID& left, const NodeID& right) {
        return closer_to(id, left, right);
    };
    std::map<NodeID, Candidate, decltype(order_ids)> candidates(order_ids);
    int confirmed_nodes = 0;
    int in_progress_endpoints = 0;
    const int MAX_NODES = 8;

    std::vector<dht::NodeContact> routing_nodes
        = _routing_table->find_closest_dht_nodes(id, MAX_NODES);

    for (auto& contact : routing_nodes) {
        Candidate candidate;
        candidate.endpoint = contact.endpoint;
        candidate.confirmed_good = false;
        candidate.in_progress = false;
        candidates[contact.id] = candidate;
    }

    const int THREADS = 3;
    WaitCondition all_done(_ios);
    ConditionVariable candidate_available(_ios);
    for (int thread = 0; thread < THREADS; thread++) {
        asio::spawn(_ios, [&, lock = all_done.lock()] (asio::yield_context yield) {
            while (true) {
                bool have_id = false;
                bool have_endpoint = false;
                NodeID candidate_id;
                udp::endpoint endpoint;

                /*
                 * Try the closest untried candidate...
                 */
                for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                    if (it->second.confirmed_good || it->second.in_progress) {
                        continue;
                    }
                    candidate_id = it->first;
                    have_id = true;
                    endpoint = it->second.endpoint;
                    have_endpoint = true;
                    it->second.in_progress = true;
                    break;
                }

                /*
                 * or, failing that, try one of the bootstrap nodes.
                 */
                if (!have_endpoint) {
                    if (!extra_starting_points.empty()) {
                        endpoint = extra_starting_points.back();
                        have_endpoint = true;
                        extra_starting_points.pop_back();
                    }
                }

                if (!have_endpoint) {
                    if (in_progress_endpoints == 0) {
                        break;
                    }
                    candidate_available.wait(yield);
                    continue;
                }
                in_progress_endpoints++;

                sys::error_code ec;

                BencodedMap find_node;
                find_node["id"] = _node_id.to_bytestring();
                find_node["target"] = id.to_bytestring();

                BencodedMap find_node_reply;
                send_query_await_reply(
                    endpoint,
                    (have_id ? boost::optional<NodeID>(candidate_id) : boost::none),
                    "find_node",
                    find_node,
                    find_node_reply,
                    std::chrono::seconds(2),
                    yield[ec]
                );

                in_progress_endpoints--;
                candidate_available.notify();

                if (ec) {
                    if (have_id) {
                        candidates.erase(candidate_id);
                    }
                    continue;
                }

                if (!find_node_reply["y"].as_string() || *find_node_reply["y"].as_string() != "r") {
                    if (have_id) {
                        candidates.erase(candidate_id);
                    }
                    continue;
                }

                boost::optional<BencodedMap> arguments = find_node_reply["r"].as_map();
                if (!arguments) {
                    if (have_id) {
                        candidates.erase(candidate_id);
                    }
                    continue;
                }

                std::vector<NodeContact> contacts;
                if (_interface_address.is_v4()) {
                    if (!(*arguments)["nodes"].is_string()) {
                        if (have_id) {
                            candidates.erase(candidate_id);
                        }
                        continue;
                    }
                    std::string encoded_contacts = *((*arguments)["nodes"].as_string());
                    // 20 bytes of ID, plus 6 bytes of endpoint
                    if (encoded_contacts.size() % 26) {
                        if (have_id) {
                            candidates.erase(candidate_id);
                        }
                        continue;
                    }
                    for (unsigned int i = 0; i < encoded_contacts.size() / 26; i++) {
                        std::string encoded_contact = encoded_contacts.substr(i * 26, 26);
                        NodeContact contact;
                        contact.id = NodeID::from_bytestring(encoded_contact.substr(0, 20));
                        contact.endpoint = *decode_endpoint(encoded_contact.substr(20));
                        contacts.push_back(contact);
                    }
                } else {
                    if (!(*arguments)["nodes6"].is_string()) {
                        if (have_id) {
                            candidates.erase(candidate_id);
                        }
                        continue;
                    }
                    std::string encoded_contacts = *((*arguments)["nodes6"].as_string());
                    // 20 bytes of ID, plus 18 bytes of endpoint
                    if (encoded_contacts.size() % 38) {
                        if (have_id) {
                            candidates.erase(candidate_id);
                        }
                        continue;
                    }
                    for (unsigned int i = 0; i < encoded_contacts.size() / 38; i++) {
                        std::string encoded_contact = encoded_contacts.substr(i * 38, 38);
                        NodeContact contact;
                        contact.id = NodeID::from_bytestring(encoded_contact.substr(0, 20));
                        contact.endpoint = *decode_endpoint(encoded_contact.substr(20));
                        contacts.push_back(contact);
                    }
                }

                /*
                 * This candidate may have been pruned in the meantime.
                 */
                if (have_id && candidates.count(candidate_id) > 0) {
                    candidates[candidate_id].confirmed_good = true;
                    candidates[candidate_id].in_progress = false;
                    confirmed_nodes++;

                    if (confirmed_nodes >= MAX_NODES) {
                        /*
                         * Remove remote candidates until there are MAX_NODES confirmed candidates
                         * left, and no nonconfirmed candidates more remote than the most remote
                         * confirmed candidate remain.
                         */
                        while (true) {
                            auto it = candidates.rbegin();
                            assert(it != candidates.rend());
                            if (it->second.confirmed_good) {
                                if (confirmed_nodes == MAX_NODES) {
                                    break;
                                } else {
                                    confirmed_nodes--;
                                }
                            }
                            candidates.erase(it->first);
                        }
                    }
                }

                bool added = false;
                for (const NodeContact& contact : contacts) {
                    if (confirmed_nodes >= MAX_NODES) {
                        if (closer_to(id, candidates.rend()->first, contact.id)) {
                            continue;
                        }
                    }

                    if (candidates.count(contact.id) > 0) {
                        continue;
                    }

                    Candidate candidate;
                    candidate.endpoint = contact.endpoint;
                    candidate.confirmed_good = false;
                    candidate.in_progress = false;
                    candidates[contact.id] = candidate;
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

void dht::DhtNode::send_ping(NodeContact contact)
{
    asio::spawn(_ios, [this, contact] (asio::yield_context yield) {
        BencodedMap ping_message;
        ping_message["id"] = _node_id.to_bytestring();

        sys::error_code ec;

        BencodedMap ping_reply;
        send_query_await_reply(
            contact.endpoint,
            contact.id,
            "ping",
            ping_message,
            ping_reply,
            std::chrono::seconds(2),
            yield[ec]
        );
    });
}



/*
 * Record a node in the routing table, space permitting. If there is no space,
 * check for node replacement opportunities. If is_verified is not set, ping
 * the target contact before adding it.
 */
void dht::DhtNode::routing_bucket_try_add_node(RoutingBucket* bucket, NodeContact contact, bool is_verified)
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
            send_ping(contact);
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
                send_ping(contact);
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
                send_ping(bucket->nodes[i].contact);
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
void dht::DhtNode::routing_bucket_fail_node(RoutingBucket* bucket, NodeContact contact)
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
            send_ping(contact);
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
        send_ping(contact);
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

std::string dht::DhtNode::encode_endpoint(udp::endpoint endpoint)
{
    std::string output;
    if (endpoint.address().is_v4()) {
        std::array<unsigned char, 4> ip_bytes = endpoint.address().to_v4().to_bytes();
        output.append((char *)ip_bytes.data(), ip_bytes.size());
    } else {
        std::array<unsigned char, 16> ip_bytes = endpoint.address().to_v6().to_bytes();
        output.append((char *)ip_bytes.data(), ip_bytes.size());
    }
    unsigned char p1 = (endpoint.port() >> 8) & 0xff;
    unsigned char p2 = (endpoint.port() >> 0) & 0xff;
    output += p1;
    output += p2;
    return output;
}

boost::optional<asio::ip::udp::endpoint> dht::DhtNode::decode_endpoint(std::string endpoint)
{
    if (endpoint.size() == 6) {
        std::array<unsigned char, 4> ip_bytes;
        std::copy(endpoint.begin(), endpoint.begin() + ip_bytes.size(), ip_bytes.data());
        uint16_t port = ((uint16_t)(unsigned char)endpoint[4]) << 8
                      | ((uint16_t)(unsigned char)endpoint[5]) << 0;
        return udp::endpoint(ip::address_v4(ip_bytes), port);
    } else if (endpoint.size() == 18) {
        std::array<unsigned char, 16> ip_bytes;
        std::copy(endpoint.begin(), endpoint.begin() + ip_bytes.size(), ip_bytes.data());
        uint16_t port = ((uint16_t)(unsigned char)endpoint[16]) << 8
                      | ((uint16_t)(unsigned char)endpoint[17]) << 0;
        return udp::endpoint(ip::address_v6(ip_bytes), port);
    } else {
        return boost::none;
    }
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
