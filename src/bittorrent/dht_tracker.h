#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>

#include <chrono>
#include <deque>
#include <string>

#include "node_id.h"

#include "../util/signal.h"

namespace ouinet {
namespace bittorrent {
namespace dht {

namespace ip = asio::ip;
using ip::tcp;

class AnnounceTokenStorage
{
    public:
    const int TOKEN_VALIDITY_SECONDS = 60 * 15;
    const int SECRET_REFRESH_TIME_SECONDS = 60 * 5;

    public:
    AnnounceTokenStorage();
    std::string generate_token(asio::ip::address address);
    bool verify_token(asio::ip::address address, const std::string& token);

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

class Swarm
{
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

class Tracker
{
    public:
    Tracker(asio::io_service& ios);
    ~Tracker();

    std::string generate_token(asio::ip::address address)
    {
        return _token_storage.generate_token(address);
    }

    bool verify_token(asio::ip::address address, const std::string& token)
    {
        return _token_storage.verify_token(address, token);
    }

    void add_peer(NodeID swarm, tcp::endpoint endpoint);
    std::vector<tcp::endpoint> list_peers(NodeID swarm, unsigned int count);

    private:
    asio::io_service& _ios;
    AnnounceTokenStorage _token_storage;
    std::map<NodeID, std::unique_ptr<Swarm>> _swarms;
    Signal<void()> _terminate_signal;
};


} // dht namespace
} // bittorrent namespace
} // ouinet namespace
