#include "dht.h"
#include "udp_multiplexer.h"
#include "code.h"
#include "collect.h"

#include "../or_throw.h"
#include "../util/condition_variable.h"
#include "../util/crypto.h"
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
    if (ec) return or_throw(yield, ec);

    _multiplexer = std::make_unique<UdpMultiplexer>(std::move(socket));

    _node_id = NodeID::zero();
    _next_transaction_id = 1;

    asio::spawn(_ios, [this] (asio::yield_context yield) {
        receive_loop(yield);
    });

    bootstrap(yield);
}

std::vector<tcp::endpoint> dht::DhtNode::tracker_get_peers(NodeID infohash, asio::yield_context yield)
{
    std::map<NodeID, TrackerNode> tracker_reply
        = tracker_search_peers(infohash, yield);

    std::vector<tcp::endpoint> peers;

    for (auto& i : tracker_reply) {
        peers.insert(peers.end(), i.second.peers.begin(), i.second.peers.end());
    }

    return peers;
}

std::vector<tcp::endpoint> dht::DhtNode::tracker_announce(NodeID infohash, boost::optional<int> port, asio::yield_context yield)
{
    std::map<NodeID, TrackerNode> tracker_reply
        = tracker_search_peers(infohash, yield);

    std::vector<tcp::endpoint> peers;

    for (auto& i : tracker_reply) {
        peers.insert(peers.end(), i.second.peers.begin(), i.second.peers.end());
    }

    for (auto& i : tracker_reply) {
        BencodedMap announce_message;
        announce_message["id"] = _node_id.to_bytestring();
        announce_message["info_hash"] = infohash.to_bytestring();
        announce_message["token"] = i.second.announce_token;
        if (port) {
            announce_message["implied_port"] = int64_t(0);
            announce_message["port"] = int64_t(*port);
        } else {
            announce_message["implied_port"] = int64_t(1);
            announce_message["port"] = int64_t(0);
        }

        send_write_query(
            i.second.node_endpoint,
            i.first,
            "announce_peer",
            announce_message
        );
    }

    return peers;
}

boost::optional<BencodedValue> dht::DhtNode::data_get_immutable(const NodeID& key, asio::yield_context yield)
{
    boost::optional<BencodedValue> data;

    collect(key, [&](const Contact& candidate , asio::yield_context yield)
                 -> boost::optional<Candidates>
        {
            if (data) return boost::none;

            sys::error_code ec;

            std::vector<NodeContact> closer_nodes;
            std::vector<NodeContact> closer_nodes6;

            boost::optional<BencodedMap> get_arguments = query_get_data(
                key,
                candidate,
                closer_nodes,
                closer_nodes6,
                yield[ec]
            );

            if (ec) return Candidates{};

            auto new_candidates = is_v4() ? closer_nodes : closer_nodes6;

            if (!get_arguments) {
                return new_candidates;
            }

            if (get_arguments->count("v")) {
                BencodedValue value = (*get_arguments)["v"];
                std::string encoded_value = bencoding_encode(value);
                std::array<uint8_t, 20> hash = util::sha1(encoded_value);
                if (hash == key.buffer) {
                    data = value;
                    return boost::none;
                }
            }

            return new_candidates;
        }
        , yield);

    return data;
}

struct DhtPutDataNode {
    asio::ip::udp::endpoint node_endpoint;
    std::string put_token;
};

NodeID dht::DhtNode::data_put_immutable(const BencodedValue& data, asio::yield_context yield)
{
    NodeID key{ util::sha1(bencoding_encode(data)) };
    std::map<NodeID, DhtPutDataNode> responsible_nodes;

    collect(key, [&](const Contact& candidate, asio::yield_context yield)
                 -> boost::optional<Candidates>
        {
            if (responsible_nodes.size() >= RESPONSIBLE_TRACKERS_PER_SWARM) {
                return boost::none;
            }

            sys::error_code ec;

            std::vector<NodeContact> closer_nodes;
            std::vector<NodeContact> closer_nodes6;

            boost::optional<BencodedMap> get_arguments = query_get_data(
                key,
                candidate,
                closer_nodes,
                closer_nodes6,
                yield[ec]
            );

            if (ec) return Candidates{};

            auto new_candidates = is_v4() ? closer_nodes : closer_nodes6;

            boost::optional<std::string> put_token = (*get_arguments)["token"].as_string();
            if (!put_token) return new_candidates;

            if (candidate.id) {
                responsible_nodes[*candidate.id] = { candidate.endpoint, *put_token };
            }

            return new_candidates;
        }
        , yield);

    size_t cnt = 0;
    for (auto& i : responsible_nodes) {
        if (cnt++ >= RESPONSIBLE_TRACKERS_PER_SWARM) break;

        // TODO: This function spawns a new coroutine internally. We should
        // wait for it to finish.
        send_write_query( i.second.node_endpoint
                        , i.first
                        , "put"
                        , { { "id", _node_id.to_bytestring() }
                          , { "v", data }
                          , { "token", i.second.put_token } });
    }

    return key;
}

static std::string mutable_data_signature_buffer(
    const BencodedValue& data,
    const std::string& salt,
    int64_t sequence_number
) {
    std::string encoded_data = bencoding_encode(data);

    /*
     * Low-level buffer computation is mandated by
     * http://bittorrent.org/beps/bep_0044.html#signature-verification
     *
     * This is a concatenation of three key/value pairs encoded as they are in
     * a BencodedMap, but in a nonstandard way, and as specified not actually
     * implemented using the BencodedMap logic.
     */
    std::string signature_buffer;
    if (!salt.empty()) {
        signature_buffer += "4:salt";
        signature_buffer += std::to_string(salt.size());
        signature_buffer += ":";
        signature_buffer += salt;
    }
    signature_buffer += "3:seqi";
    signature_buffer += std::to_string(sequence_number);
    signature_buffer += "e1:v";
    signature_buffer += encoded_data;
    return signature_buffer;
}

static bool mutable_data_valid_signature(
    BencodedMap get_arguments,
    const util::Ed25519PublicKey& public_key,
    const std::string& salt
) {
    std::array<uint8_t, 32> public_key_buffer = public_key.serialize();
    std::string public_key_string((char*)public_key_buffer.data(), public_key_buffer.size());

    if (get_arguments["k"] != public_key_string) {
        return false;
    }

    boost::optional<int64_t> sequence_number = get_arguments["seq"].as_int();
    if (!sequence_number) {
        return false;
    }

    boost::optional<std::string> signature = get_arguments["sig"].as_string();
    if (!signature) {
        return false;
    }
    if (signature->size() != 64) {
        return false;
    }
    std::array<uint8_t, 64> signature_data;
    std::copy(signature->begin(), signature->end(), signature_data.begin());

    std::string signature_buffer = mutable_data_signature_buffer(
        get_arguments["v"],
        salt,
        *sequence_number
    );
    return public_key.verify(signature_buffer, signature_data);
}

boost::optional<BencodedValue> dht::DhtNode::data_get_mutable(
    const util::Ed25519PublicKey& public_key,
    const std::string& salt,
    asio::yield_context yield
) {
    std::array<uint8_t, 32> public_key_array = public_key.serialize();
    std::string public_key_string((char*)public_key_array.data(), public_key_array.size());
    NodeID target_id{util::sha1(public_key_string + salt)};

    size_t valid_responses = 0;
    boost::optional<BencodedValue> data;
    boost::optional<int64_t> highest_sequence_number;

    collect(target_id, [&](const Contact& candidate, asio::yield_context yield)
                       -> boost::optional<Candidates>
        {
            if (valid_responses >= RESPONSIBLE_TRACKERS_PER_SWARM) {
                return boost::none;
            }

            sys::error_code ec;

            std::vector<NodeContact> closer_nodes;
            std::vector<NodeContact> closer_nodes6;

            boost::optional<BencodedMap> get_arguments = query_get_data(
                target_id,
                candidate,
                closer_nodes,
                closer_nodes6,
                yield[ec]
            );

            if (ec) return Candidates{};

            auto new_candidates = is_v4() ? closer_nodes : closer_nodes6;

            if (!get_arguments) return new_candidates;

            if (mutable_data_valid_signature(*get_arguments, public_key, salt)) {
                boost::optional<int64_t> sequence_number = (*get_arguments)["seq"].as_int();
                if (!sequence_number) {
                    return new_candidates;
                }

                if (!highest_sequence_number || *sequence_number > *highest_sequence_number) {
                    highest_sequence_number = *sequence_number;
                    data = (*get_arguments)["v"];
                    valid_responses++;
                }
            }

            return new_candidates;
        }
        , yield);

    return data;
}

NodeID dht::DhtNode::data_put_mutable(
    const BencodedValue& data,
    const util::Ed25519PrivateKey& private_key,
    const std::string& salt,
    int64_t sequence_number,
    asio::yield_context yield
) {
    std::string signature_buffer = mutable_data_signature_buffer(data, salt, sequence_number);
    std::array<uint8_t, 64> signature_array = private_key.sign(signature_buffer);
    std::string signature_string((char*)signature_array.data(), signature_array.size());

    util::Ed25519PublicKey public_key(private_key.public_key());
    std::array<uint8_t, 32> public_key_array = public_key.serialize();
    std::string public_key_string((char*)public_key_array.data(), public_key_array.size());
    NodeID target_id{util::sha1(public_key_string + salt)};

    std::map<NodeID, DhtPutDataNode> responsible_nodes;
    std::map<NodeID, DhtPutDataNode> outdated_nodes;

    collect(target_id, [&](const Contact& candidate, asio::yield_context yield)
                       -> boost::optional<Candidates>
        {
            if (responsible_nodes.size() >= RESPONSIBLE_TRACKERS_PER_SWARM) {
                return boost::none;
            }

            sys::error_code ec;

            std::vector<NodeContact> closer_nodes;
            std::vector<NodeContact> closer_nodes6;

            boost::optional<BencodedMap> get_arguments = query_get_data(
                target_id,
                candidate,
                closer_nodes,
                closer_nodes6,
                yield[ec]
            );

            if (ec) return Candidates{};

            auto new_candidates = is_v4() ? closer_nodes : closer_nodes6;

            if (!get_arguments) return new_candidates;

            boost::optional<std::string> put_token = (*get_arguments)["token"].as_string();
            if (!put_token) return new_candidates;

            DhtPutDataNode data_node{ candidate.endpoint, *put_token };

            if (mutable_data_valid_signature(*get_arguments, public_key, salt)) {
                boost::optional<int64_t> existing_sequence_number = (*get_arguments)["seq"].as_int();
                if (existing_sequence_number && *existing_sequence_number < sequence_number) {
                    /*
                     * This node has an old version of this data entry.
                     * Update it even if it is no longer responsible.
                     */
                    if (candidate.id) {
                        outdated_nodes[*candidate.id] = data_node;
                    }
                }
            }

            if (candidate.id) {
                responsible_nodes[*candidate.id] = std::move(data_node);
            }

            return new_candidates;
        }
        , yield);

    // XXX: With C++17 we could use std::map::merge
    for (auto& i : outdated_nodes) {
        responsible_nodes.insert(i);
    }

    for (auto& i : responsible_nodes) {
        BencodedMap put_message { { "id", _node_id.to_bytestring() }
                                , { "k", public_key_string }
                                , { "seq", sequence_number }
                                , { "sig", signature_string }
                                , { "v", data }
                                , { "token", i.second.put_token }};

        if (!salt.empty()) {
            put_message["salt"] = salt;
        }

        send_write_query(
            i.second.node_endpoint,
            i.first,
            "put",
            put_message
        );
    }

    return target_id;
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
    Contact dst,
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
        = { dst.endpoint
          , [&] (const BencodedMap& response_) {
                if (first_error_code) return;
                first_error_code = sys::error_code(); // success;
                response = response_;
                timeout_timer.cancel();
            }
          };

    sys::error_code ec;

    send_query( dst.endpoint
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

    return or_throw(yield, *first_error_code, std::move(response));
}

void dht::DhtNode::handle_query( udp::endpoint sender
                               , BencodedMap query
                               , asio::yield_context yield)
{
    assert(query["y"] == "q");

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
        { bootstrap_ep, boost::none },
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

    _bootstrap_endpoints.push_back(bootstrap_ep);
    /*
     * Lookup our own ID, constructing a basic path to ourselves.
     */
    find_closest_nodes(_node_id, yield);

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
                            find_closest_nodes(range.random_id(), yield);
                        });
        });

    wc.wait(yield);
}

template<class Evaluate>
void dht::DhtNode::collect( const NodeID& target_id
                          , Evaluate&& evaluate
                          , asio::yield_context yield) const
{
    // (Note: can't use lambda because we need default constructibility now)
    struct Compare {
        NodeID target_id;

        // Bootstrap nodes (those with id == boost::none) shall be ordered
        // last.
        bool operator()(const Contact& l, const Contact& r) const {
            if (!l.id && !r.id) return l.endpoint < r.endpoint;
            if ( l.id && !r.id) return true;
            if (!l.id &&  r.id) return false;
            return closer_to(target_id, *l.id, *r.id);
        }
    };

    using CandidateSet = std::set<Contact, Compare>;

    CandidateSet seed_candidates(Compare{target_id});

    std::set<udp::endpoint> added_endpoints;

    auto table_contacts = _routing_table->find_closest_routing_nodes
                              ( target_id
                              , RESPONSIBLE_TRACKERS_PER_SWARM);

    for (auto& contact : table_contacts) {
        seed_candidates.insert({ contact.endpoint, contact.id });
        added_endpoints.insert(contact.endpoint);
    }

    for (auto ep : _bootstrap_endpoints) {
        if (added_endpoints.count(ep) != 0) continue;
        seed_candidates.insert({ ep, boost::none });
    }

    ::ouinet::bittorrent::collect( _ios
                                 , std::move(seed_candidates)
                                 , std::forward<Evaluate>(evaluate)
                                 , yield);
}

std::vector<dht::NodeContact> dht::DhtNode::find_closest_nodes(
    NodeID target_id,
    asio::yield_context yield
) {
    std::set<NodeContact> output_set;

    collect(target_id, [&](const Contact& candidate, asio::yield_context yield)
                       -> boost::optional<Candidates>
        {
            if (output_set.size() >= RESPONSIBLE_TRACKERS_PER_SWARM) {
                return boost::none;
            }

            sys::error_code ec;

            std::vector<NodeContact> result_nodes;
            std::vector<NodeContact> result_nodes6;

            bool accepted = query_find_node( target_id
                                           , candidate
                                           , result_nodes
                                           , result_nodes6
                                           , yield[ec]);

            if (ec) return Candidates{};

            if (accepted && candidate.id) {
                output_set.insert({ *candidate.id, candidate.endpoint });
            }

            return is_v4() ? result_nodes : result_nodes6;
        }
        , yield);

    return { output_set.begin()
           , std::next( output_set.begin()
                      , std::min( output_set.size()
                                , RESPONSIBLE_TRACKERS_PER_SWARM))};
}

void dht::DhtNode::send_ping(NodeContact contact)
{
    // It is currently expected that this function returns immediately, due to
    // that we need to spawn an unlimited number of coroutines.  Perhaps it
    // would be better if functions using this send_ping function would only
    // spawn a limited number of coroutines and use only that.
    asio::spawn(_ios, [&, c = std::move(contact)]
                      (asio::yield_context yield) {
        sys::error_code ec;

        // Note that even though we're not explicitly using the reply here,
        // it's still being used internally by the `send_query_await_reply`
        // function to update validity of the contact inside the routing table.
        send_query_await_reply( { contact.endpoint, contact.id }
                              , "ping"
                              , BencodedMap{{ "id", _node_id.to_bytestring() }}
                              , std::chrono::seconds(2)
                              , yield[ec]);
    });
}

/*
 * Send a query that writes data to the DHT. Repeat up to 5 times until we
 * get a positive response. Return immediately without waiting for results.
 */
void dht::DhtNode::send_write_query(
    udp::endpoint destination,
    NodeID destination_id,
    const std::string& query_type,
    const BencodedMap& query_arguments
) {
    asio::spawn(_ios, [=] (asio::yield_context yield) {
        /*
         * Retry the write message a couple of times.
         */
        const int TRIES = 5;
        for (int i = 0; i < TRIES; i++) {
            sys::error_code ec;

            BencodedMap write_reply = send_query_await_reply(
                { destination, destination_id },
                query_type,
                query_arguments,
                std::chrono::seconds(5),
                yield[ec]
            );

            if (!ec) {
                break;
            }
        }
    });
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
    std::vector<NodeContact>& closer_nodes6,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap find_node_reply = send_query_await_reply(
        node,
        "find_node",
        BencodedMap { { "id", _node_id.to_bytestring() }
                    , { "target", target_id.to_bytestring() } },
        std::chrono::seconds(2),
        yield[ec]
    );

    if (ec) {
        return false;
    }

    if (find_node_reply["y"] != "r") return false;

    boost::optional<BencodedMap> arguments = find_node_reply["r"].as_map();
    if (!arguments) {
        return false;
    }

    boost::optional<std::string> nodes  = (*arguments)["nodes"].as_string();
    boost::optional<std::string> nodes6 = (*arguments)["nodes6"].as_string();

    if (nodes) {
        if (!decode_contacts_v4(*nodes,  closer_nodes)) {
            return false;
        }
    }

    if (nodes6) {
        if (!decode_contacts_v6(*nodes6, closer_nodes6)) {
            return false;
        }
    }

    return (is_v4() && !closer_nodes .empty())
        || (is_v6() && !closer_nodes6.empty());
}

boost::optional<BencodedMap> dht::DhtNode::query_get_data(
    NodeID key,
    Contact node,
    std::vector<NodeContact>& closer_nodes,
    std::vector<NodeContact>& closer_nodes6,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap get_reply = send_query_await_reply(
        node,
        "get",
        BencodedMap { { "id", _node_id.to_bytestring() }
                    , { "target", key.to_bytestring() } },
        std::chrono::seconds(2),
        yield[ec]
    );

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
            closer_nodes6,
            yield[ec]
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
            closer_nodes6,
            yield[ec]
        );
        return boost::none;
    }

    boost::optional<BencodedMap> get_arguments = get_reply["r"].as_map();
    if (!get_arguments) {
        return boost::none;
    }

    boost::optional<std::string> nodes  = (*get_arguments)["nodes"].as_string();
    boost::optional<std::string> nodes6 = (*get_arguments)["nodes6"].as_string();

    if (nodes) {
        if (!decode_contacts_v4(*nodes,  closer_nodes)) {
            return boost::none;
        }
    }

    if (nodes6) {
        if (!decode_contacts_v6(*nodes6, closer_nodes6)) {
            return boost::none;
        }
    }

    return *get_arguments;
}

// http://bittorrent.org/beps/bep_0005.html#get-peers
boost::optional<dht::DhtNode::TrackerNode>
dht::DhtNode::query_get_peers( NodeID infohash
                             , Contact node
                             , std::vector<NodeContact>& closer_nodes
                             , std::vector<NodeContact>& closer_nodes6
                             , asio::yield_context yield)
{
    sys::error_code ec;

    BencodedMap get_peers_reply = send_query_await_reply(
        node,
        "get_peers",
        BencodedMap { { "id", _node_id.to_bytestring() }
                    , { "info_hash", infohash.to_bytestring() } },
        std::chrono::seconds(2),
        yield[ec]
    );

    if (ec) {
        return or_throw(yield, ec, boost::none);
    }

    if (get_peers_reply["y"] != "r") {
        return boost::none;
    }

    boost::optional<BencodedMap> get_peers_arguments = get_peers_reply["r"].as_map();
    if (!get_peers_arguments) {
        return boost::none;
    }

    boost::optional<BencodedList> encoded_peers = (*get_peers_arguments)["values"].as_list();
    boost::optional<std::string> announce_token = (*get_peers_arguments)["token"].as_string();

    boost::optional<TrackerNode> tracker;

    if (encoded_peers && announce_token) {
        tracker = TrackerNode {
            .node_endpoint = node.endpoint,
            .peers = {},
            .announce_token = *announce_token
        };

        for (auto& peer : *encoded_peers) {
            boost::optional<std::string> peer_string = peer.as_string();
            if (!peer_string) continue;

            boost::optional<udp::endpoint> endpoint = decode_endpoint(*peer_string);
            if (!endpoint) continue;

            tracker->peers.push_back({endpoint->address(), endpoint->port()});
        }
    }

    boost::optional<std::string> nodes  = (*get_peers_arguments)["nodes"].as_string();
    boost::optional<std::string> nodes6 = (*get_peers_arguments)["nodes6"].as_string();

    if (nodes) {
        if (!decode_contacts_v4(*nodes,  closer_nodes))
            return boost::none;
    }

    if (nodes6) {
        if (!decode_contacts_v6(*nodes6, closer_nodes6))
            return boost::none;
    }

    return tracker;
}

/**
 * Perform a get_peers search. Returns the peers found, as well as necessary
 * data to later perform an announce operation.
 */
std::map<NodeID, dht::DhtNode::TrackerNode>
dht::DhtNode::tracker_search_peers(
    NodeID infohash,
    asio::yield_context yield
) {
    std::map<NodeID, TrackerNode> tracker_reply;

    collect(infohash, [&](const Contact& candidate, asio::yield_context yield)
                       -> boost::optional<Candidates>
        {
            if (tracker_reply.size() >= RESPONSIBLE_TRACKERS_PER_SWARM) {
                return boost::none;
            }

            sys::error_code ec;

            std::vector<NodeContact> result_nodes;
            std::vector<NodeContact> result_nodes6;

            auto opt_tracker = query_get_peers( infohash
                                              , candidate
                                              , result_nodes
                                              , result_nodes6
                                              , yield[ec]);

            if (ec) {
                return Candidates{};
            }

            if (opt_tracker && candidate.id) {
                tracker_reply.insert({ *candidate.id , std::move(*opt_tracker) });
            }

            auto* contacts = is_v4() ? &result_nodes : &result_nodes6;

            if (contacts->empty()) {
                // XXX: Is this necessary? I.e. if the candidate knew, wouldn't
                // it have replied in the get_peers message in the first place?
                query_find_node( infohash
                               , candidate
                               , result_nodes
                               , result_nodes6
                               , yield[ec]);

                contacts = is_v4() ? &result_nodes : &result_nodes6;
            }

            return *contacts;
        }
        , yield);

    tracker_reply.erase( std::next( tracker_reply.begin()
                                  , std::min( tracker_reply.size()
                                            , RESPONSIBLE_TRACKERS_PER_SWARM))
                       , tracker_reply.end());

    return tracker_reply;
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

std::vector<tcp::endpoint> MainlineDht::tracker_get_peers( NodeID infohash
                                                         , asio::yield_context yield)
{
    WaitCondition wc(_ios);

    std::vector<tcp::endpoint> retval;

    for (auto& node : _nodes) {
        asio::spawn(_ios, [&, lock = wc.lock()] (asio::yield_context yield) {
                sys::error_code ec;
                auto trackers = node.second->tracker_get_peers(infohash, yield[ec]);
                if (ec) return;
                retval.insert(retval.end(), trackers.begin(), trackers.end());
            });
    }

    wc.wait(yield);
    return retval;
}

void MainlineDht::set_interfaces( const std::vector<asio::ip::address>& addresses
                                , asio::yield_context yield)
{
    WaitCondition wc(_ios);

    std::set<asio::ip::address> addresses_used;

    for (asio::ip::address address : addresses) {
        addresses_used.insert(address);

        if (!_nodes.count(address)) {
            auto node = std::make_unique<dht::DhtNode>(_ios, address);

            asio::spawn(_ios, [&, n = std::move(node), lock = wc.lock()]
                              (asio::yield_context yield) mutable {
                sys::error_code ec;
                n->start(yield[ec]);
                if (!ec) _nodes[address] = std::move(n);
            });
        }
    }

    wc.wait(yield);

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
