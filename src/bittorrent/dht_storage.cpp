#include "dht_storage.h"

#include "../async_sleep.h"
#include "../util/bytes.h"
#include "../util/crypto.h"

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
    auto hash = util::sha1(secret + address.to_string() + id.to_bytestring());
    return std::string((char *)hash.data(), hash.size());
}

bool detail::DhtWriteTokenStorage::verify_token(asio::ip::address address, NodeID id, const std::string& token)
{
    expire();

    for (auto& i : _secrets) {
        auto hash = util::sha1(i.secret + address.to_string() + id.to_bytestring());
        if (std::string((char *)hash.data(), hash.size()) == token) {
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



DataStore::DataStore(asio::io_service& ios):
    _ios(ios)
{
    /*
     * Every so often, remove expired data items.
     */
    asio::spawn(_ios, [this] (asio::yield_context yield) {
        while (true) {
            if (!async_sleep(_ios, std::chrono::seconds(60), _terminate_signal, yield)) {
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
    return util::sha1(bencoding_encode(value));
}

boost::optional<DataStore::PutError> DataStore::put_immutable(BencodedValue value)
{
    /*
     * Size limit specified in BEP 44
     */
    if (bencoding_encode(value).size() >= 1000) {
        return PutError::ValueTooLarge;
    }

    _immutable_data[immutable_get_id(value)] = ImmutableStoredItem {
        value,
        std::chrono::steady_clock::now()
    };

    return boost::none;
}

boost::optional<BencodedValue> DataStore::get_immutable(NodeID id)
{
    auto it = _immutable_data.find(id);
    if (it == _immutable_data.end()) {
        return boost::none;
    }
    return it->second.value;
}

NodeID DataStore::mutable_get_id(util::Ed25519PublicKey public_key, const std::string& salt)
{
    return util::sha1(util::bytes::to_string(public_key.serialize()) + salt);
}

boost::optional<DataStore::PutError> DataStore::put_mutable(MutableDataItem item)
{
    /*
     * Size limits specified in BEP 44
     */
    if (bencoding_encode(item.value).size() >= 1000) {
        return PutError::ValueTooLarge;
    }
    if (item.salt.size() > 64) {
        return PutError::SaltTooLarge;
    }

    _mutable_data[mutable_get_id(item.public_key, item.salt)] = MutableStoredItem {
        item,
        std::chrono::steady_clock::now()
    };

    return boost::none;
}

boost::optional<DataStore::MutableDataItem> DataStore::get_mutable(NodeID id)
{
    auto it = _mutable_data.find(id);
    if (it == _mutable_data.end()) {
        return boost::none;
    }
    return it->second.item;
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

std::array<uint8_t, 64> DataStore::sign_mutable(
    BencodedValue value,
    int64_t sequence_number,
    const std::string& salt,
    util::Ed25519PrivateKey private_key
) {
    return private_key.sign(
        mutable_data_signature_buffer(value, salt, sequence_number)
    );
}

bool DataStore::verify_mutable(MutableDataItem item)
{
    return item.public_key.verify(
        mutable_data_signature_buffer(
            item.value,
            item.salt,
            item.sequence_number
        ),
        item.signature
    );
}

} // dht namespace
} // bittorrent namespace
} // ouinet namespace
