#include "dht.h"
#include "udp_multiplexer.h"
#include "code.h"
#include "collect.h"
#include "proximity_map.h"

#include "../async_sleep.h"
#include "../or_throw.h"
#include "../util/bytes.h"
#include "../util/condition_variable.h"
#include "../util/crypto.h"
#include "../util/success_condition.h"
#include "../util/wait_condition.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <set>

#include <iostream>

namespace ouinet {
namespace bittorrent {

using dht::NodeContact;
using Candidates = std::vector<NodeContact>;

#define DEBUG_SHOW_MESSAGES 0

std::string dht::NodeContact::to_string() const
{
    return id.to_hex() + " at " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}


dht::DhtNode::DhtNode(asio::io_service& ios, ip::address interface_address):
    _ios(ios),
    _interface_address(interface_address),
    _tracker(std::make_unique<Tracker>(_ios)),
    _data_store(std::make_unique<DataStore>(_ios)),
    _ready(false)
{
}

void dht::DhtNode::start(asio::yield_context yield)
{
    sys::error_code ec;

    udp::socket socket(_ios);

    if (_interface_address.is_v4()) {
        socket.open(udp::v4(), ec);
    } else {
        socket.open(udp::v6(), ec);
    }
    if (ec) {
        return or_throw(yield, ec);
    }

    udp::endpoint endpoint(_interface_address, 0);
    socket.bind(endpoint, ec);
    if (ec) {
        return or_throw(yield, ec);
    }

    _multiplexer = std::make_unique<UdpMultiplexer>(std::move(socket));

    _node_id = NodeID::zero();
    _next_transaction_id = 1;

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        receive_loop(yield);
    });

    bootstrap(yield[ec]);
    if (ec) {
        return or_throw(yield, ec);
    }

    /*
     * For each bucket in the routing table, lookup a random ID in that range.
     * This ensures that every node that should route to us, knows about us.
     * This can be done after bootstrap proper, in the background.
     */
    refresh_routing_table();
}

void dht::DhtNode::stop()
{
    _multiplexer = nullptr;
    _terminate_signal();
}

dht::DhtNode::~DhtNode()
{
    stop();
}

std::set<tcp::endpoint> dht::DhtNode::tracker_get_peers(
    NodeID infohash,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;
    std::set<tcp::endpoint> peers;
    std::map<NodeID, TrackerNode> responsible_nodes;
    tracker_do_search_peers(infohash, peers, responsible_nodes, yield[ec], cancel_signal);
    return or_throw<std::set<tcp::endpoint>>(yield, ec, std::move(peers));
}

std::set<tcp::endpoint> dht::DhtNode::tracker_announce(
    NodeID infohash,
    boost::optional<int> port,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;
    std::set<tcp::endpoint> peers;
    std::map<NodeID, TrackerNode> responsible_nodes;
    tracker_do_search_peers(infohash, peers, responsible_nodes, yield[ec], cancel_signal);
    if (ec) {
        return or_throw<std::set<tcp::endpoint>>(yield, ec, std::move(peers));
    }

    bool success = false;
    WaitCondition wc(_ios);
    for (auto& i : responsible_nodes) {
        asio::spawn(_ios, [&, lock = wc.lock()] (asio::yield_context yield) {
            sys::error_code ec;
            send_write_query(
                i.second.node_endpoint,
                i.first,
                "announce_peer",
                BencodedMap {
                    { "id", _node_id.to_bytestring() },
                    { "info_hash", infohash.to_bytestring() },
                    { "token", i.second.announce_token },
                    { "implied_port", port ? int64_t(0) : int64_t(1) },
                    { "port", port ? int64_t(*port) : int64_t(0) }
                },
                yield[ec],
                cancel_signal
            );
            if (!ec) {
                success = true;
            }
        });
    }
    wc.wait(yield);

    ec = success ? sys::error_code() : boost::asio::error::network_down;

    return or_throw<std::set<tcp::endpoint>>(yield, ec, std::move(peers));
}

boost::optional<BencodedValue> dht::DhtNode::data_get_immutable(
    const NodeID& key,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;
    /*
     * This is a ProximitySet, really.
     */
    ProximityMap<boost::none_t> responsible_nodes(key, RESPONSIBLE_TRACKERS_PER_SWARM);
    boost::optional<BencodedValue> data;

    collect(key, [&](
        const Contact& candidate,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) -> boost::optional<Candidates> {
        if (!candidate.id && responsible_nodes.full()) {
            return boost::none;
        }
        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return boost::none;
        }

        /*
         * As soon as we have found a valid data value, we can stop the search.
         */
        if (data) {
            return boost::none;
        }

        std::vector<NodeContact> closer_nodes;
        boost::optional<BencodedMap> response_ = query_get_data(
            key,
            candidate,
            closer_nodes,
            yield,
            cancel_signal
        );
        if (!response_) {
            return closer_nodes;
        }
        BencodedMap& response = *response_;

        if (candidate.id) {
            responsible_nodes.insert({ *candidate.id, boost::none });
        }

        if (response.count("v")) {
            BencodedValue value = response["v"];
            if (_data_store->immutable_get_id(value) == key) {
                data = value;
                return boost::none;
            }
        }

        return closer_nodes;
    }, yield[ec], cancel_signal);

    return or_throw<boost::optional<BencodedValue>>(yield, ec, std::move(data));
}

NodeID dht::DhtNode::data_put_immutable(
    const BencodedValue& data,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    NodeID key = _data_store->immutable_get_id(data);

    sys::error_code ec;
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes(key, RESPONSIBLE_TRACKERS_PER_SWARM);

    collect(key, [&](
        const Contact& candidate,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) -> boost::optional<Candidates> {
        if (!candidate.id && responsible_nodes.full()) {
            return boost::none;
        }
        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return boost::none;
        }

        std::vector<NodeContact> closer_nodes;
        boost::optional<BencodedMap> response_ = query_get_data(
            key,
            candidate,
            closer_nodes,
            yield,
            cancel_signal
        );
        if (!response_) {
            return closer_nodes;
        }
        BencodedMap& response = *response_;

        boost::optional<std::string> put_token = response["token"].as_string();
        if (!put_token) {
            return closer_nodes;
        }

        if (candidate.id) {
            responsible_nodes.insert({
                *candidate.id,
                { candidate.endpoint, *put_token }
            });
        }

        return closer_nodes;
    }, yield[ec], cancel_signal);

    if (ec) {
        return or_throw<NodeID>(yield, ec, std::move(key));
    }

    bool success = false;
    WaitCondition wc(_ios);
    for (auto& i : responsible_nodes) {
        asio::spawn(_ios, [&, lock = wc.lock()] (asio::yield_context yield) {
            sys::error_code ec;
            send_write_query(
                i.second.node_endpoint,
                i.first,
                "put",
                {
                    { "id", _node_id.to_bytestring() },
                    { "v", data },
                    { "token", i.second.put_token }
                },
                yield[ec],
                cancel_signal
            );
            if (!ec) {
                success = true;
            }
        });
    }
    wc.wait(yield);

    ec = success ? sys::error_code() : boost::asio::error::network_down;

    return or_throw<NodeID>(yield, ec, std::move(key));
}

boost::optional<MutableDataItem> dht::DhtNode::data_get_mutable(
    const util::Ed25519PublicKey& public_key,
    boost::string_view salt,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    NodeID target_id = _data_store->mutable_get_id(public_key, salt);

    sys::error_code ec;
    /*
     * This is a ProximitySet, really.
     */
    ProximityMap<boost::none_t> responsible_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);
    boost::optional<MutableDataItem> data;

    collect(target_id, [&](
        const Contact& candidate,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) -> boost::optional<Candidates> {
        if (!candidate.id && responsible_nodes.full()) {
            return boost::none;
        }
        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return boost::none;
        }

        /*
         * We want to find the latest version of the data, so don't stop early.
         */

        std::vector<NodeContact> closer_nodes;
        boost::optional<BencodedMap> response_ = query_get_data(
            target_id,
            candidate,
            closer_nodes,
            yield,
            cancel_signal
        );
        if (!response_) {
            return closer_nodes;
        }
        BencodedMap& response = *response_;

        if (candidate.id) {
            responsible_nodes.insert({ *candidate.id, boost::none });
        }

        if (response["k"] != util::bytes::to_string(public_key.serialize())) {
            return closer_nodes;
        }
        boost::optional<int64_t> sequence_number = response["seq"].as_int();
        if (!sequence_number) {
            return closer_nodes;
        }
        boost::optional<std::string> signature = response["sig"].as_string();
        if (!signature || signature->size() != 64) {
            return closer_nodes;
        }

        MutableDataItem item {
            public_key,
            salt.to_string(),
            response["v"],
            *sequence_number,
            util::bytes::to_array<uint8_t, 64>(*signature)
        };
        if (item.verify()) {
            if (!data || *sequence_number > data->sequence_number) {
                data = item;
            }
        }

        return closer_nodes;
    }, yield[ec], cancel_signal);

    return or_throw<boost::optional<MutableDataItem>>(yield, ec, std::move(data));
}

NodeID dht::DhtNode::data_put_mutable(
    MutableDataItem data,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    NodeID target_id = _data_store->mutable_get_id(data.public_key, data.salt);

    sys::error_code ec;
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);
    std::map<NodeID, ResponsibleNode> outdated_nodes;

    collect(target_id, [&](
        const Contact& candidate,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) -> boost::optional<Candidates> {
        if (!candidate.id && responsible_nodes.full()) {
            return boost::none;
        }

        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return boost::none;
        }

        std::vector<NodeContact> closer_nodes;
        boost::optional<BencodedMap> response_ = query_get_data(
            target_id,
            candidate,
            closer_nodes,
            yield,
            cancel_signal
        );
        if (!response_) {
            return closer_nodes;
        }
        BencodedMap& response = *response_;

        boost::optional<std::string> put_token = response["token"].as_string();
        if (!put_token) {
            return closer_nodes;
        }

        ResponsibleNode data_node{ candidate.endpoint, *put_token };
        if (candidate.id) {
            responsible_nodes.insert({*candidate.id, std::move(data_node)});
        }

        if (response["k"] != util::bytes::to_string(data.public_key.serialize())) {
            return closer_nodes;
        }
        boost::optional<int64_t> existing_sequence_number = response["seq"].as_int();
        if (!existing_sequence_number) {
            return closer_nodes;
        }
        boost::optional<std::string> existing_signature = response["sig"].as_string();
        if (!existing_signature || existing_signature->size() != 64) {
            return closer_nodes;
        }

        MutableDataItem item {
            data.public_key,
            data.salt,
            response["v"],
            *existing_sequence_number,
            util::bytes::to_array<uint8_t, 64>(*existing_signature)
        };
        if (item.verify()) {
            if (*existing_sequence_number < data.sequence_number) {
                /*
                 * This node has an old version of this data entry.
                 * Update it even if it is no longer responsible.
                 */
                if (candidate.id) {
                    outdated_nodes[*candidate.id] = data_node;
                }
            }
        }

        return closer_nodes;
    }, yield[ec], cancel_signal);

    if (ec) {
        return or_throw<NodeID>(yield, ec, std::move(target_id));
    }

    std::map<NodeID, ResponsibleNode*> all_nodes;

    for (auto& i : responsible_nodes) { all_nodes.insert({i.first, &i.second}); }
    for (auto& i : outdated_nodes)    { all_nodes.insert({i.first, &i.second}); }

    bool success = false;
    WaitCondition wc(_ios);
    for (auto& i : all_nodes) {
        asio::spawn(_ios, [&, lock = wc.lock()] (asio::yield_context yield) {
            BencodedMap put_message {
                { "id", _node_id.to_bytestring() },
                { "k", util::bytes::to_string(data.public_key.serialize()) },
                { "seq", data.sequence_number },
                { "sig", util::bytes::to_string(data.signature) },
                { "v", data.value },
                { "token", i.second->put_token }
            };

            if (!data.salt.empty()) {
                put_message["salt"] = data.salt;
            }

            sys::error_code ec;
            send_write_query(
                i.second->node_endpoint,
                i.first,
                "put",
                put_message,
                yield[ec],
                cancel_signal
            );
            if (!ec) {
                success = true;
            }
        });
    }
    wc.wait(yield);

    ec = success ? sys::error_code() : boost::asio::error::network_down;

    return or_throw<NodeID>(yield, ec, std::move(target_id));
}

NodeID dht::DhtNode::data_put_mutable(
    const BencodedValue& data,
    const util::Ed25519PrivateKey& private_key,
    const std::string& salt,
    int64_t sequence_number,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    return data_put_mutable(MutableDataItem::sign(
        data,
        sequence_number,
        salt,
        private_key
    ), yield, cancel_signal);
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
            = _multiplexer->receive(sender, yield[ec], _terminate_signal);

        if (ec) {
            break;
        }

        // TODO: The bencode parser should only need a string_view.
        boost::optional<BencodedValue> decoded_message
            = bencoding_decode(packet.to_string());

        if (!decoded_message) {
#           if DEBUG_SHOW_MESSAGES
            std::cerr << "recv: " << sender
                      << " Failed parsing \"" << packet << "\"" << std::endl;
#           endif

            continue;
        }

#       if DEBUG_SHOW_MESSAGES
        std::cerr << "recv: " << sender << " " << *decoded_message << std::endl;
#       endif

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

void dht::DhtNode::send_datagram(
    udp::endpoint destination,
    const BencodedMap& message
) {
#   if DEBUG_SHOW_MESSAGES
    std::cerr << "send: " << destination << " " << message << std::endl;
#   endif
    _multiplexer->send(std::move(bencoding_encode(message)), destination);
}

void dht::DhtNode::send_datagram(
    udp::endpoint destination,
    const BencodedMap& message,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
#   if DEBUG_SHOW_MESSAGES
    std::cerr << "send: " << destination << " " << message << std::endl;
#   endif
    _multiplexer->send(std::move(bencoding_encode(message)), destination, yield, cancel_signal);
}

void dht::DhtNode::send_query(
    udp::endpoint destination,
    std::string transaction,
    std::string query_type,
    BencodedMap query_arguments,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    send_datagram(
        destination,
        BencodedMap {
            { "y", "q" },
            { "q", std::move(query_type) },
            { "a", std::move(query_arguments) },
            // TODO: version string
            { "t", std::move(transaction) }
        },
        yield,
        cancel_signal
    );
}

/*
 * Send a query message to a destination, and wait for either a reply, an error
 * reply, or a timeout.
 *
 * If destination_id is set, update the routing table in accordance with
 * whether a successful reply was received.
 */
BencodedMap dht::DhtNode::send_query_await_reply(
    Contact dst,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    asio::steady_timer::duration timeout,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
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

    auto cancel_slot = cancel_signal.connect([&] {
        first_error_code = asio::error::operation_aborted;
        timeout_timer.cancel();
    });

    bool terminated = false;
    auto terminate_slot = _terminate_signal.connect([&] {
        terminated = true;
        first_error_code = asio::error::operation_aborted;
        timeout_timer.cancel();
    });

    std::string transaction = new_transaction_string();

    _active_requests[transaction] = {
        dst.endpoint,
        [&] (const BencodedMap& response_) {
            /*
             * This function is never called when the Dht object is
             * destructed, thus the terminate_slot.
             */
            if (first_error_code) {
                return;
            }
            first_error_code = sys::error_code(); // success;
            response = response_;
            timeout_timer.cancel();
        }
    };

    sys::error_code ec;
    send_query(
        dst.endpoint,
        transaction,
        std::move(query_type),
        std::move(query_arguments),
        yield[ec],
        cancel_signal
    );
    if (ec) {
        first_error_code = ec;
        timeout_timer.cancel();
    }

    reply_and_timeout_condition.wait(yield);

    if (terminated) {
        return or_throw<BencodedMap>(yield, asio::error::operation_aborted);
    }

    /*
     * We do this cleanup when cancelling the operation, but NOT when
     * the Dht object has been destroyed.
     */
    _active_requests.erase(transaction);

    if (first_error_code && *first_error_code == asio::error::operation_aborted) {
        return or_throw<BencodedMap>(yield, *first_error_code);
    }

    if (dst.id) {
        NodeContact contact{ .id = *dst.id, .endpoint = dst.endpoint };

        if (*first_error_code || response["y"] != "r") {
            /*
             * Record the failure in the routing table.
             */
            dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(*dst.id, false);
            routing_bucket_fail_node(routing_bucket, contact);
        } else {
            /*
             * Add the node to the routing table, subject to space limitations.
             */
            dht::RoutingBucket* routing_bucket = _routing_table->find_bucket(*dst.id, true);
            routing_bucket_try_add_node(routing_bucket, contact, true);
        }
    }

    return or_throw<BencodedMap>(yield, *first_error_code, std::move(response));
}

void dht::DhtNode::handle_query(udp::endpoint sender, BencodedMap query)
{
    assert(query["y"] == "q");

    boost::optional<std::string> transaction_ = query["t"].as_string();

    if (!transaction_) { return; }

    std::string transaction = *transaction_;

    auto send_error = [&] (int code, std::string description) {
        send_datagram(
            sender,
            BencodedMap {
                { "y", "e" },
                { "t", transaction },
                { "e", BencodedList{code, description} }
            }
        );
    };

    auto send_reply = [&] (BencodedMap reply) {
        reply["id"] = _node_id.to_bytestring();

        send_datagram(
            sender,
            BencodedMap {
                { "y", "r" },
                { "t", transaction },
                { "e", std::move(reply) }
            }
        );
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
        if (is_v4()) {
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
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        reply["token"] = _tracker->generate_token(sender.address(), infohash);

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
                if (infohash.closer_to(_node_id, i.id)) {
                    contains_self = true;
                }
            }
            if (!contains_self) {
                send_error(201, "This torrent is not my responsibility");
                return;
            }
        }

        if (!_tracker->verify_token(sender.address(), infohash, token)) {
            send_error(203, "Incorrect announce token");
            return;
        }

        _tracker->add_peer(infohash, tcp::endpoint(sender.address(), effective_port));

        BencodedMap reply;
        send_reply(reply);
        return;
    } else if (query_type == "get") {
        boost::optional<std::string> target_ = arguments["target"].as_string();
        if (!target_) {
            send_error(203, "Missing argument 'target'");
            return;
        }
        if (target_->size() != 20) {
            send_error(203, "Malformed argument 'target'");
            return;
        }
        NodeID target = NodeID::from_bytestring(*target_);

        boost::optional<int64_t> sequence_number_ = arguments["seq"].as_int();

        BencodedMap reply;

        std::vector<dht::NodeContact> contacts
            = _routing_table->find_closest_routing_nodes(target, RoutingBucket::BUCKET_SIZE);
        std::string nodes;
        for (auto& contact : contacts) {
            nodes += contact.id.to_bytestring();
            nodes += encode_endpoint(contact.endpoint);
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        reply["token"] = _data_store->generate_token(sender.address(), target);

        if (!sequence_number_) {
            boost::optional<BencodedValue> immutable_value = _data_store->get_immutable(target);
            if (immutable_value) {
                reply["v"] = *immutable_value;
                send_reply(reply);
                return;
            }
        }

        boost::optional<MutableDataItem> mutable_item = _data_store->get_mutable(target);
        if (mutable_item) {
            if (sequence_number_ && *sequence_number_ <= mutable_item->sequence_number) {
                send_reply(reply);
                return;
            }

            reply["k"] = util::bytes::to_string(mutable_item->public_key.serialize());
            reply["seq"] = mutable_item->sequence_number;
            reply["sig"] = util::bytes::to_string(mutable_item->signature);
            reply["v"] = mutable_item->value;
            send_reply(reply);
            return;
        }

        send_reply(reply);
        return;
    } else if (query_type == "put") {
        boost::optional<std::string> token_ = arguments["token"].as_string();
        if (!token_) {
            send_error(203, "Missing argument 'token'");
            return;
        }

        if (!arguments.count("v")) {
            send_error(203, "Missing argument 'v'");
            return;
        }
        BencodedValue value = arguments["v"];
        /*
         * Size limit specified in BEP 44
         */
        if (bencoding_encode(value).size() >= 1000) {
            send_error(205, "Argument 'v' too big");
            return;
        }

        if (arguments["k"].is_string()) {
            /*
             * This is a mutable data item.
             */
            boost::optional<std::string> public_key_ = arguments["k"].as_string();
            if (!public_key_) {
                send_error(203, "Missing argument 'k'");
                return;
            }
            if (public_key_->size() != 32) {
                send_error(203, "Malformed argument 'k'");
                return;
            }
            util::Ed25519PublicKey public_key(util::bytes::to_array<uint8_t, 32>(*public_key_));

            boost::optional<std::string> signature_ = arguments["sig"].as_string();
            if (!signature_) {
                send_error(203, "Missing argument 'sig'");
                return;
            }
            if (signature_->size() != 64) {
                send_error(203, "Malformed argument 'sig'");
                return;
            }
            std::array<uint8_t, 64> signature = util::bytes::to_array<uint8_t, 64>(*signature_);

            boost::optional<int64_t> sequence_number_ = arguments["seq"].as_int();
            if (!sequence_number_) {
                send_error(203, "Missing argument 'seq'");
                return;
            }
            int64_t sequence_number = *sequence_number_;

            boost::optional<std::string> salt_ = arguments["salt"].as_string();
            /*
             * Size limit specified in BEP 44
             */
            if (salt_ && salt_->size() > 64) {
                send_error(207, "Argument 'salt' too big");
                return;
            }
            std::string salt = salt_ ? *salt_ : "";

            NodeID target = _data_store->mutable_get_id(public_key, salt);

            if (!_data_store->verify_token(sender.address(), target, *token_)) {
                send_error(203, "Incorrect put token");
                return;
            }

            /*
             * Reject put requests for which there are more than enough
             * better responsible known nodes.
             *
             * TODO: This can be done in a more efficient way once the routing
             * table code stabilizes.
             */
            {
                bool contains_self = false;
                std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(target, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
                for (auto& i : closer_nodes) {
                    if (target.closer_to(_node_id, i.id)) {
                        contains_self = true;
                    }
                }
                if (!contains_self) {
                    send_error(201, "This data item is not my responsibility");
                    return;
                }
            }

            MutableDataItem item {
                public_key,
                salt,
                value,
                sequence_number,
                signature
            };
            if (!item.verify()) {
                send_error(206, "Invalid signature");
                return;
            }

            boost::optional<MutableDataItem> existing_item = _data_store->get_mutable(target);
            if (existing_item) {
                if (sequence_number < existing_item->sequence_number) {
                    send_error(302, "Sequence number less than current");
                    return;
                }

                if (
                       sequence_number == existing_item->sequence_number
                    && bencoding_encode(value) != bencoding_encode(existing_item->value)
                ) {
                    send_error(302, "Sequence number not updated");
                    return;
                }

                boost::optional<int64_t> compare_and_swap_ = arguments["cas"].as_int();
                if (compare_and_swap_ && *compare_and_swap_ != existing_item->sequence_number) {
                    send_error(301, "Compare-and-swap mismatch");
                    return;
                }
            }

            _data_store->put_mutable(item);

            BencodedMap reply;
            send_reply(reply);
            return;
        } else {
            /*
             * This is an immutable data item.
             */
            NodeID target = _data_store->immutable_get_id(value);

            if (!_data_store->verify_token(sender.address(), target, *token_)) {
                send_error(203, "Incorrect put token");
                return;
            }

            /*
             * Reject put requests for which there are more than enough
             * better responsible known nodes.
             *
             * TODO: This can be done in a more efficient way once the routing
             * table code stabilizes.
             */
            {
                bool contains_self = false;
                std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(target, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
                for (auto& i : closer_nodes) {
                    if (target.closer_to(_node_id, i.id)) {
                        contains_self = true;
                    }
                }
                if (!contains_self) {
                    send_error(201, "This data item is not my responsibility");
                    return;
                }
            }

            _data_store->put_immutable(value);

            BencodedMap reply;
            send_reply(reply);
            return;
        }
    } else {
        send_error(204, "Query type not implemented");
        return;
    }
}


static
asio::ip::udp::endpoint resolve(
    asio::io_context& ioc,
    const std::string& addr,
    const std::string& port,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    using asio::ip::udp;

    sys::error_code ec;

    udp::resolver::query bootstrap_query(addr, port);
    udp::resolver bootstrap_resolver(ioc);

    auto cancel_slot = cancel_signal.connect([&] {
        bootstrap_resolver.cancel();
    });

    udp::resolver::iterator it = bootstrap_resolver.async_resolve(bootstrap_query, yield[ec]);

    if (ec) {
        return or_throw<udp::endpoint>(yield, ec);
    }

    if (it != udp::resolver::iterator()) {
        return it->endpoint();
    }

    return or_throw<udp::endpoint>(yield, asio::error::not_found);
}

void dht::DhtNode::bootstrap(asio::yield_context yield)
{
    sys::error_code ec;

    // Other servers include router.utorrent.com:6881 and dht.transmissionbt.com:6881
    auto bootstrap_ep = resolve(
        _ios,
        "router.bittorrent.com",
        "6881",
        yield[ec],
        _terminate_signal
    );

    if (ec == asio::error::operation_aborted) {
        return or_throw(yield, ec);
    }
    if (ec) {
        std::cout << "Unable to resolve bootstrap server, giving up\n";
        return or_throw(yield, ec);
    }

    BencodedMap initial_ping_message;
    initial_ping_message["id"] = _node_id.to_bytestring();

    BencodedMap initial_ping_reply = send_query_await_reply(
        { bootstrap_ep, boost::none },
        "ping",
        initial_ping_message,
        std::chrono::seconds(15),
        yield[ec],
        _terminate_signal
    );
    if (ec == asio::error::operation_aborted) {
        return or_throw(yield, ec);
    }
    if (ec) {
        std::cout << "Bootstrap server does not reply, giving up\n";
        return or_throw(yield, ec);
    }

    boost::optional<std::string> my_ip = initial_ping_reply["ip"].as_string();
    if (!my_ip) {
        std::cout << "Unexpected bootstrap server reply, giving up\n";
        return or_throw(yield, ec);
    }
    boost::optional<asio::ip::udp::endpoint> my_endpoint = decode_endpoint(*my_ip);
    if (!my_endpoint) {
        std::cout << "Unexpected bootstrap server reply, giving up\n";
        return or_throw(yield, ec);
    }

    _node_id = NodeID::generate(my_endpoint->address());
    _wan_endpoint = *my_endpoint;
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

    _bootstrap_endpoints.push_back(bootstrap_ep);

    /*
     * Lookup our own ID, constructing a basic path to ourselves.
     */
    find_closest_nodes(_node_id, yield[ec], _terminate_signal);
    if (ec) {
        return or_throw(yield, ec);
    }

    /*
     * We now know enough nodes that general DHT queries should succeed. The
     * remaining work is part of our participation in the DHT, but is not
     * necessary for implementing queries.
     */
    _ready = true;
}


void dht::DhtNode::refresh_routing_table()
{
    _routing_table->for_each_bucket([&] (
        const NodeID::Range& range,
        RoutingBucket& bucket
    ) {
        spawn(_ios, [this, range] (asio::yield_context yield) {
            sys::error_code ec;
            find_closest_nodes(range.random_id(), yield[ec], _terminate_signal);
        });
    });
}

template<class Evaluate>
void dht::DhtNode::collect(
    const NodeID& target_id,
    Evaluate&& evaluate,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) const {
    if (!_routing_table) {
        // We're not yet bootstrapped.
        return or_throw(yield, asio::error::try_again);
    }

    // (Note: can't use lambda because we need default constructibility now)
    struct Compare {
        NodeID target_id;

        // Bootstrap nodes (those with id == boost::none) shall be ordered
        // last.
        bool operator()(const Contact& l, const Contact& r) const {
            if (!l.id && !r.id) return l.endpoint < r.endpoint;
            if ( l.id && !r.id) return true;
            if (!l.id &&  r.id) return false;
            return target_id.closer_to(*l.id, *r.id);
        }
    };

    using CandidateSet = std::set<Contact, Compare>;

    CandidateSet seed_candidates(Compare{target_id});

    std::set<udp::endpoint> added_endpoints;

    auto table_contacts =
        _routing_table->find_closest_routing_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    for (auto& contact : table_contacts) {
        seed_candidates.insert(contact);
        added_endpoints.insert(contact.endpoint);
    }

    for (auto ep : _bootstrap_endpoints) {
        if (added_endpoints.count(ep) != 0) continue;
        seed_candidates.insert({ ep, boost::none });
    }

    ::ouinet::bittorrent::collect(
        _ios,
        std::move(seed_candidates),
        std::forward<Evaluate>(evaluate),
        yield,
        cancel_signal
    );
}

std::vector<dht::NodeContact> dht::DhtNode::find_closest_nodes(
    NodeID target_id,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;
    ProximityMap<udp::endpoint> out(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    collect(target_id, [&](
        const Contact& candidate,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) -> boost::optional<Candidates> {
        if (!candidate.id && out.full()) {
            return boost::none;
        }

        if (candidate.id && !out.would_insert(*candidate.id)) {
            return boost::none;
        }

        std::vector<NodeContact> closer_nodes;
        bool accepted = query_find_node(
            target_id,
            candidate,
            closer_nodes,
            yield,
            cancel_signal
        );

        if (accepted && candidate.id) {
            out.insert({ *candidate.id, candidate.endpoint });
        }

        return closer_nodes;
    }
    , yield[ec], cancel_signal);

    std::vector<NodeContact> output_set;
    for (auto& c : out) {
        output_set.push_back({ c.first, c.second });
    }

    return or_throw<std::vector<dht::NodeContact>>(yield, ec, std::move(output_set));
}

void dht::DhtNode::send_ping(NodeContact contact)
{
    // It is currently expected that this function returns immediately, due to
    // that we need to spawn an unlimited number of coroutines.  Perhaps it
    // would be better if functions using this send_ping function would only
    // spawn a limited number of coroutines and use only that.
    asio::spawn(_ios, [this, contact] (asio::yield_context yield) {
        sys::error_code ec;
        Signal<void()> cancel_signal;

        // Note that even though we're not explicitly using the reply here,
        // it's still being used internally by the `send_query_await_reply`
        // function to update validity of the contact inside the routing table.
        send_query_await_reply(
            contact,
            "ping",
            BencodedMap{{ "id", _node_id.to_bytestring() }},
            std::chrono::seconds(2),
            yield[ec],
            cancel_signal
        );
    });
}

/*
 * Send a query that writes data to the DHT. Repeat up to 5 times until we
 * get a positive response.
 */
void dht::DhtNode::send_write_query(
    udp::endpoint destination,
    NodeID destination_id,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    /*
     * Retry the write message a couple of times.
     */
    const int TRIES = 5;
    sys::error_code ec;
    for (int i = 0; i < TRIES; i++) {
        BencodedMap write_reply = send_query_await_reply(
            { destination, destination_id },
            query_type,
            query_arguments,
            std::chrono::seconds(5),
            yield[ec],
            cancel_signal
        );

        if (!ec) {
            break;
        }
    }
    or_throw(yield, ec);
}

/**
 * Send a find_node query to a target node, and parse the reply.
 * @return True when received a valid response, false otherwise.
 */
// http://bittorrent.org/beps/bep_0005.html#find-node
bool dht::DhtNode::query_find_node(
    NodeID target_id,
    Contact node,
    std::vector<NodeContact>& closer_nodes,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;

    BencodedMap find_node_reply = send_query_await_reply(
        node,
        "find_node",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", target_id.to_bytestring() }
        },
        std::chrono::seconds(2),
        yield[ec],
        cancel_signal
    );

    if (ec) {
        return false;
    }
    if (find_node_reply["y"] != "r") {
        return false;
    }
    boost::optional<BencodedMap> response = find_node_reply["r"].as_map();
    if (!response) {
        return false;
    }

    if (is_v4()) {
        boost::optional<std::string> nodes = (*response)["nodes"].as_string();
        if (!decode_contacts_v4(*nodes, closer_nodes)) {
            return false;
        }
    } else {
        boost::optional<std::string> nodes6 = (*response)["nodes6"].as_string();
        if (!decode_contacts_v6(*nodes6, closer_nodes)) {
            return false;
        }
    }

    return !closer_nodes.empty();
}

// http://bittorrent.org/beps/bep_0005.html#get-peers
boost::optional<BencodedMap> dht::DhtNode::query_get_peers(
    NodeID infohash,
    Contact node,
    std::vector<NodeContact>& closer_nodes,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;

    BencodedMap get_peers_reply = send_query_await_reply(
        node,
        "get_peers",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "info_hash", infohash.to_bytestring() }
        },
        std::chrono::seconds(2),
        yield[ec],
        cancel_signal
    );

    if (ec) {
        return boost::none;
    }
    if (get_peers_reply["y"] != "r") {
        return boost::none;
    }
    boost::optional<BencodedMap> response = get_peers_reply["r"].as_map();
    if (!response) {
        return boost::none;
    }

    if (is_v4()) {
        boost::optional<std::string> nodes = (*response)["nodes"].as_string();
        if (!decode_contacts_v4(*nodes, closer_nodes)) {
            return boost::none;
        }
    } else {
        boost::optional<std::string> nodes6 = (*response)["nodes6"].as_string();
        if (!decode_contacts_v6(*nodes6, closer_nodes)) {
            return boost::none;
        }
    }

    if (closer_nodes.empty()) {
        /*
         * We got a reply to get_peers, but it does not contain nodes.
         * Follow up with a find_node to fill the gap.
         */
        query_find_node(
            infohash,
            node,
            closer_nodes,
            yield,
            cancel_signal
        );
    }

    return response;
}

// http://bittorrent.org/beps/bep_0044.html#get-message
boost::optional<BencodedMap> dht::DhtNode::query_get_data(
    NodeID key,
    Contact node,
    std::vector<NodeContact>& closer_nodes,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;

    BencodedMap get_reply = send_query_await_reply(
        node,
        "get",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", key.to_bytestring() }
        },
        std::chrono::seconds(2),
        yield[ec],
        cancel_signal
    );

    if (ec == asio::error::operation_aborted) {
        return boost::none;
    }

    if (ec) {
        /*
         * Ideally, nodes that do not implement BEP 44 would reply to this
         * query with a "not implemented" error. But in practice, most do not
         * reply at all. If such nodes make up the entire routing table (as is
         * often the case), the lookup might fail entirely. But doing an entire
         * search through nodes without BEP 44 support slows things down quite
         * a lot. Hm.
         *
         * TODO: Perhaps using a separate routing table for BEP 44 nodes would
         * improve things here?
         */
        query_find_node(
            key,
            node,
            closer_nodes,
            yield,
            cancel_signal
        );
        return boost::none;
    }

    if (get_reply["y"] != "r") {
        /*
         * This is probably a node that does not implement BEP 44.
         * Query it using find_node instead. Ignore errors and hope for
         * the best; we are just trying to find some closer nodes here.
         */
        query_find_node(
            key,
            node,
            closer_nodes,
            yield,
            cancel_signal
        );
        return boost::none;
    }

    boost::optional<BencodedMap> response = get_reply["r"].as_map();
    if (!response) {
        return boost::none;
    }

    if (is_v4()) {
        boost::optional<std::string> nodes = (*response)["nodes"].as_string();
        if (!decode_contacts_v4(*nodes, closer_nodes)) {
            return boost::none;
        }
    } else {
        boost::optional<std::string> nodes6 = (*response)["nodes6"].as_string();
        if (!decode_contacts_v6(*nodes6, closer_nodes)) {
            return boost::none;
        }
    }

    return response;
}

/**
 * Perform a get_peers search. Returns the peers found, as well as necessary
 * data to later perform an announce operation.
 */
void dht::DhtNode::tracker_do_search_peers(
    NodeID infohash,
    std::set<tcp::endpoint>& peers,
    std::map<NodeID, TrackerNode>& responsible_nodes,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    sys::error_code ec;
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::vector<tcp::endpoint> peers;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes_full(infohash, RESPONSIBLE_TRACKERS_PER_SWARM);

    collect(infohash, [&](
        const Contact& candidate,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) -> boost::optional<Candidates> {
        if (!candidate.id && responsible_nodes_full.full()) {
            return boost::none;
        }
        if (candidate.id && !responsible_nodes_full.would_insert(*candidate.id)) {
            return boost::none;
        }

        std::vector<NodeContact> closer_nodes;
        boost::optional<BencodedMap> response_ = query_get_peers(
            infohash,
            candidate,
            closer_nodes,
            yield,
            cancel_signal
        );
        if (!response_) {
            return closer_nodes;
        }
        BencodedMap& response = *response_;

        boost::optional<std::string> announce_token = response["token"].as_string();
        if (!announce_token) {
            return closer_nodes;
        }

        if (candidate.id) {
            ResponsibleNode node{ candidate.endpoint, {}, *announce_token };
            boost::optional<BencodedList> encoded_peers = response["values"].as_list();
            if (encoded_peers) {
                for (auto& peer : *encoded_peers) {
                    boost::optional<std::string> peer_string = peer.as_string();
                    if (!peer_string) {
                        continue;
                    }
                    boost::optional<udp::endpoint> endpoint = decode_endpoint(*peer_string);
                    if (!endpoint) {
                        continue;
                    }
                    node.peers.push_back({endpoint->address(), endpoint->port()});
                }
            }
            responsible_nodes_full.insert({ *candidate.id, std::move(node) });
        }

        return closer_nodes;
    }, yield[ec], cancel_signal);

    peers.clear();
    responsible_nodes.clear();
    for (auto& i : responsible_nodes_full) {
        peers.insert(i.second.peers.begin(), i.second.peers.end());
        responsible_nodes[i.first] = { i.second.node_endpoint, i.second.put_token };
    }

    or_throw(yield, ec);
}



/*
 * Record a node in the routing table, space permitting. If there is no space,
 * check for node replacement opportunities. If is_verified is not set, ping
 * the target contact before adding it.
 */
void dht::DhtNode::routing_bucket_try_add_node( RoutingBucket* bucket
                                              , NodeContact contact
                                              , bool is_verified)
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
void dht::DhtNode::routing_bucket_fail_node( RoutingBucket* bucket
                                           , NodeContact contact)
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



MainlineDht::MainlineDht(asio::io_service& ios)
    : _ios(ios)
{
    /*
     * Refresh publications periodically.
     */
    asio::spawn(_ios, [this] (asio::yield_context yield) {
        while (true) {
            if (!async_sleep(_ios, std::chrono::seconds(60), _terminate_signal, yield)) {
                break;
            }

            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

            /*
             * TODO: This needs proper cancellation support, as soon as DhtNode supports that.
             */

            for (auto& i : _publications.tracker_publications) {
                if (i.second.last_sent + std::chrono::seconds(_publications.ANNOUNCE_INTERVAL_SECONDS) < now) {
                    i.second.last_sent = now;
                    for (auto& j : _nodes) {
                        asio::spawn(_ios, [&] (asio::yield_context yield) {
                            Signal<void()> cancel_signal;
                            auto terminate_slot = _terminate_signal.connect([&] {
                                cancel_signal();
                            });
                            j.second->tracker_announce(i.first, i.second.port, yield, cancel_signal);
                        });
                    }
                }
            }

            for (auto& i : _publications.immutable_publications) {
                if (i.second.last_sent + std::chrono::seconds(_publications.PUT_INTERVAL_SECONDS) < now) {
                    i.second.last_sent = now;
                    for (auto& j : _nodes) {
                        asio::spawn(_ios, [&] (asio::yield_context yield) {
                            Signal<void()> cancel_signal;
                            auto terminate_slot = _terminate_signal.connect([&] {
                                cancel_signal();
                            });
                            j.second->data_put_immutable(i.second.data, yield, cancel_signal);
                        });
                    }
                }
            }

            for (auto& i : _publications.mutable_publications) {
                if (i.second.last_sent + std::chrono::seconds(_publications.PUT_INTERVAL_SECONDS) < now) {
                    i.second.last_sent = now;
                    for (auto& j : _nodes) {
                        asio::spawn(_ios, [&] (asio::yield_context yield) {
                            Signal<void()> cancel_signal;
                            auto terminate_slot = _terminate_signal.connect([&] {
                                cancel_signal();
                            });
                            j.second->data_put_mutable(i.second.data, yield, cancel_signal);
                        });
                    }
                }
            }
        }
    });

}

MainlineDht::~MainlineDht()
{
    _terminate_signal();
}

void MainlineDht::set_interfaces(const std::vector<asio::ip::address>& addresses)
{
    std::set<asio::ip::address> addresses_used;
    addresses_used.insert(addresses.begin(), addresses.end());

    for (auto it = _nodes.begin(); it != _nodes.end(); ) {
        if (addresses_used.count(it->first)) {
            ++it;
        } else {
            it = _nodes.erase(it);
        }
    }

    for (asio::ip::address address : addresses_used) {
        if (!_nodes.count(address)) {
            asio::spawn(_ios, [&, address] (asio::yield_context yield) mutable {
                _nodes[address] = std::make_unique<dht::DhtNode>(_ios, address);
                sys::error_code ec;
                _nodes[address]->start(yield[ec]);
            });
        }
    }
}

std::set<tcp::endpoint> MainlineDht::tracker_announce_start(
    NodeID infohash,
    boost::optional<int> port,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    dht::DhtPublications::TrackerPublication publication { port, std::chrono::steady_clock::now() };
    _publications.tracker_publications[infohash] = publication;

    std::set<tcp::endpoint> output;

    SuccessCondition condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [&, lock = condition.lock()] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            Signal<void()> cancel_dummy;
            std::set<tcp::endpoint> peers = i.second->tracker_announce(infohash, port, yield[ec], cancel_dummy);

            if (ec) {
                return;
            }

            output.insert(peers.begin(), peers.end());

            /*
             * TODO: We should distinguish here between
             * "did not query successfully" and "did not find any peers".
             * This needs error detection in _announce(), which does not exist.
             */
            if (peers.size()) {
                lock.release(true);
            }
        });
    }
    auto cancel_slot = cancel_signal.connect([&] {
        condition.cancel();
    });
    if (!condition.wait_for_success(yield)) {
        if (condition.cancelled()) {
            return or_throw<std::set<tcp::endpoint>>(yield, asio::error::operation_aborted, std::move(output));
        } else {
            return or_throw<std::set<tcp::endpoint>>(yield, asio::error::network_unreachable);
        }
    }

    return output;
}

void MainlineDht::tracker_announce_start(
    NodeID infohash,
    boost::optional<int> port
) {
    dht::DhtPublications::TrackerPublication publication { port, std::chrono::steady_clock::now() };
    _publications.tracker_publications[infohash] = publication;

    for (auto& i : _nodes) {
        asio::spawn(_ios, [&] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            Signal<void()> cancel_dummy;
            std::set<tcp::endpoint> peers = i.second->tracker_announce(infohash, port, yield[ec], cancel_dummy);
        });
    }
}

void MainlineDht::tracker_announce_stop(NodeID infohash)
{
    _publications.tracker_publications.erase(infohash);
}

NodeID MainlineDht::immutable_put_start(
    const BencodedValue& data,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    NodeID key = dht::DataStore::immutable_get_id(data);
    dht::DhtPublications::ImmutablePublication publication { data, std::chrono::steady_clock::now() };
    _publications.immutable_publications[key] = publication;

    SuccessCondition condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [&, lock = condition.lock()] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            Signal<void()> cancel_dummy;
            i.second->data_put_immutable(data, yield[ec], cancel_dummy);

            if (ec) {
                return;
            }

            lock.release(true);
        });
    }
    auto cancel_slot = cancel_signal.connect([&] {
        condition.cancel();
    });
    if (!condition.wait_for_success(yield)) {
        if (condition.cancelled()) {
            return or_throw<NodeID>(yield, asio::error::operation_aborted);
        } else {
            return or_throw<NodeID>(yield, asio::error::network_unreachable);
        }
    }

    return key;
}

NodeID MainlineDht::immutable_put_start(
    const BencodedValue& data
) {
    NodeID key = dht::DataStore::immutable_get_id(data);
    dht::DhtPublications::ImmutablePublication publication { data, std::chrono::steady_clock::now() };
    _publications.immutable_publications[key] = publication;

    for (auto& i : _nodes) {
        asio::spawn(_ios, [&] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            Signal<void()> cancel_dummy;
            i.second->data_put_immutable(data, yield[ec], cancel_dummy);
        });
    }

    return key;
}

void MainlineDht::immutable_put_stop(NodeID key)
{
    _publications.immutable_publications.erase(key);
}

NodeID MainlineDht::mutable_put_start(
    const MutableDataItem& data,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    NodeID key = dht::DataStore::mutable_get_id(data.public_key, data.salt);
    dht::DhtPublications::MutablePublication publication { data, std::chrono::steady_clock::now() };
    _publications.mutable_publications[key] = publication;

    SuccessCondition condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [&, lock = condition.lock()] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            Signal<void()> cancel_dummy;
            i.second->data_put_mutable(data, yield[ec], cancel_dummy);

            if (ec) {
                return;
            }

            lock.release(true);
        });
    }
    auto cancel_slot = cancel_signal.connect([&] {
        condition.cancel();
    });
    if (!condition.wait_for_success(yield)) {
        if (condition.cancelled()) {
            return or_throw<NodeID>(yield, asio::error::operation_aborted);
        } else {
            return or_throw<NodeID>(yield, asio::error::network_unreachable);
        }
    }

    return key;
}

NodeID MainlineDht::mutable_put_start(
    const MutableDataItem& data
) {
    NodeID key = dht::DataStore::mutable_get_id(data.public_key, data.salt);
    dht::DhtPublications::MutablePublication publication { data, std::chrono::steady_clock::now() };
    _publications.mutable_publications[key] = publication;

    for (auto& i : _nodes) {
        asio::spawn(_ios, [&] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            Signal<void()> cancel_dummy;
            i.second->data_put_mutable(data, yield[ec], cancel_dummy);
        });
    }

    return key;
}

void MainlineDht::mutable_put_stop(NodeID key)
{
    _publications.mutable_publications.erase(key);
}

std::set<tcp::endpoint> MainlineDht::tracker_get_peers(NodeID infohash, asio::yield_context yield, Signal<void()>& cancel_signal)
{
    std::set<tcp::endpoint> output;
    sys::error_code ec;

    Signal<void()> cancel_attempts;

    SuccessCondition success_condition(_ios);
    WaitCondition completed_condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            std::set<tcp::endpoint> peers = i.second->tracker_get_peers(infohash, yield[ec], cancel_attempts);

            output.insert(peers.begin(), peers.end());

            if (peers.size()) {
                success.release(true);
            }
        });
    }

    auto cancel_slot = cancel_signal.connect([&] {
        success_condition.cancel();
        cancel_attempts();
    });
    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::network_unreachable;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw<std::set<tcp::endpoint>>(yield, ec);
}

boost::optional<BencodedValue> MainlineDht::immutable_get(NodeID key, asio::yield_context yield, Signal<void()>& cancel_signal)
{
    boost::optional<BencodedValue> output;
    sys::error_code ec;

    Signal<void()> cancel_attempts;

    SuccessCondition success_condition(_ios);
    WaitCondition completed_condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            boost::optional<BencodedValue> data = i.second->data_get_immutable(key, yield[ec], cancel_attempts);

            if (!ec && data) {
                output = data;
                success.release(true);
            }
        });
    }
    auto cancel_slot = cancel_signal.connect([&] {
        success_condition.cancel();
        cancel_attempts();
    });
    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::network_unreachable;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw<boost::optional<BencodedValue>>(yield, ec);
}

boost::optional<MutableDataItem> MainlineDht::mutable_get(
    const util::Ed25519PublicKey& public_key,
    boost::string_view salt,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    boost::optional<MutableDataItem> output;
    sys::error_code ec;

    Signal<void()> cancel_attempts;

    SuccessCondition success_condition(_ios);
    WaitCondition completed_condition(_ios);
    for (auto& i : _nodes) {
        asio::spawn(_ios, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            boost::optional<MutableDataItem> data = i.second->data_get_mutable(
                public_key,
                salt,
                yield[ec],
                cancel_attempts
            );

            if (!ec && data) {
                output = data;
                success.release(true);
            }
        });
    }
    auto cancel_slot = cancel_signal.connect([&] {
        success_condition.cancel();
        cancel_attempts();
    });
    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::network_unreachable;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw<boost::optional<MutableDataItem>>(yield, ec);
}


} // bittorrent namespace
} // ouinet namespace
