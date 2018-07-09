#include "dht_tracker.h"

#include "../async_sleep.h"
#include "../util/crypto.h"

#include <cstdlib>

using namespace ouinet::bittorrent::dht;

AnnounceTokenStorage::AnnounceTokenStorage():
    _salt(util::random(32))
{}

std::string AnnounceTokenStorage::generate_token(asio::ip::address address)
{
    expire();
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    if (_secrets.empty() || now > _last_generated + std::chrono::seconds(SECRET_REFRESH_TIME_SECONDS)) {
        Secret secret;
        secret.secret = std::to_string(now.time_since_epoch().count()) + _salt;
        secret.expires = now + std::chrono::seconds(TOKEN_VALIDITY_SECONDS);
        _secrets.push_back(secret);
    }

    std::string secret = _secrets.back().secret;
    auto hash = util::sha1(secret + address.to_string());
    return std::string((char *)hash.data(), hash.size());
}

bool AnnounceTokenStorage::verify_token(asio::ip::address address, const std::string& token)
{
    expire();

    for (auto& i : _secrets) {
        auto hash = util::sha1(i.secret + address.to_string());
        if (std::string((char *)hash.data(), hash.size()) == token) {
            return true;
        }
    }

    return false;
}

void AnnounceTokenStorage::expire()
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    while (!_secrets.empty() && _secrets[0].expires < now) {
        _secrets.pop_front();
    }
}



void Swarm::add(tcp::endpoint endpoint)
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    auto it = _peer_indices.find(endpoint);
    if (it == _peer_indices.end()) {
        Peer peer;
        peer.endpoint = endpoint;
        peer.last_seen = now;
        _peers.push_back(peer);
        _peer_indices[endpoint] = _peers.size();
    } else {
        size_t index = it->second;
        assert(_peers[index].endpoint == endpoint);
        _peers[index].last_seen = now;
    }
}

/*
 * This function must return a _random_ selection of endpoints.
 * Which is why Swarm needs this complicated data structure.
 */
std::vector<tcp::endpoint> Swarm::list(unsigned int count)
{
    std::vector<tcp::endpoint> output;
    for (size_t i = 0; i < count && i < _peers.size(); i++) {
        /*
         * (1) select a peer outside the range [0..i);
         * (2) swap it with _peers[i];
         * (3) update the peer index accordingly.
         */
        size_t target = i + std::rand() % (_peers.size() - i);
        output.push_back(_peers[target].endpoint);
        if (target != i) {
            std::swap(_peer_indices[_peers[target].endpoint], _peer_indices[_peers[i].endpoint]);
            std::swap(_peers[target], _peers[i]);
        }
    }
    return output;
}

void Swarm::expire()
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    size_t i = 0;
    while (i < _peers.size()) {
        if (_peers[i].last_seen + std::chrono::seconds(ANNOUNCE_VALIDITY_SECONDS) < now) {
            size_t replacement = _peers.size() - 1;
            if (replacement != i) {
                std::swap(_peer_indices[_peers[replacement].endpoint], _peer_indices[_peers[i].endpoint]);
                std::swap(_peers[replacement], _peers[i]);
            }
            _peer_indices.erase(_peers[replacement].endpoint);
            _peers.pop_back();
        } else {
            i++;
        }
    }
}



Tracker::Tracker(asio::io_service& ios):
    _ios(ios)
{
    /*
     * Every so often, remove expired peers from swarms.
     */
    asio::spawn(_ios, [this] (asio::yield_context yield) {
        while (true) {
            if (!async_sleep(_ios, std::chrono::seconds(60), _terminate_signal, yield)) {
                break;
            }

            auto it = _swarms.begin();
            while (it != _swarms.end()) {
                it->second->expire();
                if (it->second->empty()) {
                    it = _swarms.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });
}

Tracker::~Tracker()
{
    _terminate_signal();
}

void Tracker::add_peer(NodeID swarm, tcp::endpoint endpoint)
{
    if (!_swarms.count(swarm)) {
        _swarms[swarm] = std::make_unique<Swarm>();
    }
    _swarms[swarm]->add(endpoint);
}

std::vector<tcp::endpoint> Tracker::list_peers(NodeID swarm, unsigned int count)
{
    if (_swarms.count(swarm)) {
        return _swarms[swarm]->list(count);
    } else {
        return std::vector<tcp::endpoint>();
    }
}
