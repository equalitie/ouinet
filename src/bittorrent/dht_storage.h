#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include <chrono>
#include <deque>
#include <string>

#include "bencoding.h"
#include "node_id.h"

#include "../util/crypto.h"
#include "../util/signal.h"

namespace ouinet {
namespace bittorrent {
namespace dht {

namespace ip = asio::ip;
using ip::tcp;

namespace detail {

class DhtWriteTokenStorage {
    public:
    const int TOKEN_VALIDITY_SECONDS = 60 * 15;
    const int SECRET_REFRESH_TIME_SECONDS = 60 * 5;

    public:
    DhtWriteTokenStorage();
    std::string generate_token(asio::ip::address address, NodeID id);
    bool verify_token(asio::ip::address address, NodeID id, const std::string& token);

    private:
    void expire();

    private:
    std::string _salt;
    struct Secret {
        std::string secret;
        std::chrono::steady_clock::time_point expires;
    };
    std::deque<Secret> _secrets;
    std::chrono::steady_clock::time_point _last_generated;
};

class Swarm {
    public:
    /*
     * This number based on vague hints. I could not find any proper
     * specification on recommended validity times, and this could be
     * completely wrong.
     */
    const int ANNOUNCE_VALIDITY_SECONDS = 3600 * 2;

    public:
    void add(tcp::endpoint endpoint);
    std::vector<tcp::endpoint> list(unsigned int count);
    void expire();
    bool empty() const { return _peers.empty(); }

    private:
    struct Peer {
        tcp::endpoint endpoint;
        std::chrono::steady_clock::time_point last_seen;
    };
    std::vector<Peer> _peers;
    std::map<tcp::endpoint, size_t> _peer_indices;
};

} // namespace detail

class Tracker {
    public:
    Tracker(asio::io_service& ios);
    ~Tracker();

    std::string generate_token(asio::ip::address address, NodeID id)
    {
        return _token_storage.generate_token(address, id);
    }

    bool verify_token(asio::ip::address address, NodeID id, const std::string& token)
    {
        return _token_storage.verify_token(address, id, token);
    }

    void add_peer(NodeID swarm, tcp::endpoint endpoint);
    std::vector<tcp::endpoint> list_peers(NodeID swarm, unsigned int count);

    private:
    asio::io_service& _ios;
    detail::DhtWriteTokenStorage _token_storage;
    std::map<NodeID, std::unique_ptr<detail::Swarm>> _swarms;
    Signal<void()> _terminate_signal;
};

class DataStore {
    public:
    /*
     * Validity specified at
     * http://www.bittorrent.org/beps/bep_0044.html#expiration
     */
    const int PUT_VALIDITY_SECONDS = 3600 * 2;

    public:
    DataStore(asio::io_service& ios);
    ~DataStore();

    std::string generate_token(asio::ip::address address, NodeID id)
    {
        return _token_storage.generate_token(address, id);
    }

    bool verify_token(asio::ip::address address, NodeID id, const std::string& token)
    {
        return _token_storage.verify_token(address, id, token);
    }

    enum class PutError
    {
        ValueTooLarge,
        SaltTooLarge,
        InvalidSignature
    };

    NodeID immutable_get_id(BencodedValue value);
    boost::optional<PutError> put_immutable(BencodedValue value);
    boost::optional<BencodedValue> get_immutable(NodeID id);

    struct MutableDataItem {
        util::Ed25519PublicKey public_key;
        std::string salt;
        BencodedValue value;
        int64_t sequence_number;
        std::array<uint8_t, 64> signature;
    };
    NodeID mutable_get_id(util::Ed25519PublicKey public_key, const std::string& salt);
    boost::optional<PutError> put_mutable(MutableDataItem item);
    boost::optional<MutableDataItem> get_mutable(NodeID id);
    std::array<uint8_t, 64> sign_mutable(
        BencodedValue value,
        int64_t sequence_number,
        const std::string& salt,
        util::Ed25519PrivateKey private_key
    );
    bool verify_mutable(MutableDataItem item);

    private:
    struct ImmutableStoredItem {
        BencodedValue value;
        std::chrono::steady_clock::time_point last_seen;
    };
    struct MutableStoredItem {
        MutableDataItem item;
        std::chrono::steady_clock::time_point last_seen;
    };

    asio::io_service& _ios;
    detail::DhtWriteTokenStorage _token_storage;
    std::map<NodeID, ImmutableStoredItem> _immutable_data;
    std::map<NodeID, MutableStoredItem> _mutable_data;
    Signal<void()> _terminate_signal;
};


} // dht namespace
} // bittorrent namespace
} // ouinet namespace
