#include "dht_storage.h"

#include "../async_sleep.h"
#include "../util/bytes.h"
#include "../util/crypto.h"
#include "../util/hash.h"

#include <cstdlib>

namespace ouinet {
namespace bittorrent {
namespace dht {

detail::DhtWriteTokenStorage::DhtWriteTokenStorage():
    _salt(util::random(32))
{}

std::string detail::DhtWriteTokenStorage::generate_token(asio::ip::address address, NodeID id)
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
    auto hash = util::sha1_digest(secret + address.to_string() + id.to_bytestring());
    return std::string((char *)hash.data(), hash.size());
}

bool detail::DhtWriteTokenStorage::verify_token(asio::ip::address address, NodeID id, const std::string& token)
{
    expire();

    for (auto& i : _secrets) {
        auto hash = util::sha1_digest(i.secret + address.to_string() + id.to_bytestring());

        if (boost::string_view((char *)hash.data(), hash.size()) == token) {
            return true;
        }
    }

    return false;
}

void detail::DhtWriteTokenStorage::expire()
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    while (!_secrets.empty() && _secrets[0].expires < now) {
        _secrets.pop_front();
    }
}



void detail::Swarm::add(tcp::endpoint endpoint)
{
    auto now = std::chrono::steady_clock::now();
    auto it = _peer_indices.find(endpoint);

    if (it == _peer_indices.end()) {
        Peer peer;
        peer.endpoint = endpoint;
        peer.last_seen = now;
        _peer_indices[endpoint] = _peers.size();
        _peers.push_back(peer);
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
std::vector<tcp::endpoint> detail::Swarm::list(unsigned int count)
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

void detail::Swarm::expire()
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



Tracker::Tracker(const asio::executor& exec):
    _exec(exec)
{
    /*
     * Every so often, remove expired peers from swarms.
     */
    asio::spawn(_exec, [this] (asio::yield_context yield) {
        auto terminated = _terminate_signal.connect([]{});

        while (true) {
            async_sleep(_exec, std::chrono::seconds(60), _terminate_signal, yield);
            if (terminated) {
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
        _swarms[swarm] = std::make_unique<detail::Swarm>();
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



DataStore::DataStore(const asio::executor& exec):
    _exec(exec)
{
    /*
     * Every so often, remove expired data items.
     */
    asio::spawn(_exec, [this] (asio::yield_context yield) {
        auto terminated = _terminate_signal.connect([]{});

        while (true) {
            async_sleep(_exec, std::chrono::seconds(60), _terminate_signal, yield);
            if (terminated) {
                break;
            }

            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

            auto it = _immutable_data.begin();
            while (it != _immutable_data.end()) {
                if (it->second.last_seen + std::chrono::seconds(PUT_VALIDITY_SECONDS) < now) {
                    it = _immutable_data.erase(it);
                } else {
                    ++it;
                }
            }

            auto i = _mutable_data.begin();
            while (i != _mutable_data.end()) {
                if (i->second.last_seen + std::chrono::seconds(PUT_VALIDITY_SECONDS) < now) {
                    i = _mutable_data.erase(i);
                } else {
                    ++i;
                }
            }
        }
    });
}

DataStore::~DataStore()
{
    _terminate_signal();
}

NodeID DataStore::immutable_get_id(BencodedValue value)
{
    return util::sha1_digest(bencoding_encode(value));
}

void DataStore::put_immutable(BencodedValue value)
{
    _immutable_data[immutable_get_id(value)] = ImmutableStoredItem {
        value,
        std::chrono::steady_clock::now()
    };
}

boost::optional<BencodedValue> DataStore::get_immutable(NodeID id)
{
    auto it = _immutable_data.find(id);
    if (it == _immutable_data.end()) {
        return boost::none;
    }
    return it->second.value;
}

NodeID DataStore::mutable_get_id( util::Ed25519PublicKey public_key
                                , boost::string_view salt)
{
    return util::sha1_digest(public_key.serialize(), salt);
}

void DataStore::put_mutable(MutableDataItem item)
{
    _mutable_data[mutable_get_id(item.public_key, item.salt)] = MutableStoredItem {
        item,
        std::chrono::steady_clock::now()
    };
}

boost::optional<MutableDataItem> DataStore::get_mutable(NodeID id)
{
    auto it = _mutable_data.find(id);
    if (it == _mutable_data.end()) {
        return boost::none;
    }
    return it->second.item;
}

} // dht namespace
} // bittorrent namespace
} // ouinet namespace
