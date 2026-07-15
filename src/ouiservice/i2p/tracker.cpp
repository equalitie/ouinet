#include "tracker.h"

#include <sstream>
#include <cstdlib>
#include <ctime>

#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>

#include "../../bittorrent/bencoding.h"
#include "../../bittorrent/node_id.h"
#include "../../logger.h"
#include "../../util/exponential_backoff.h"
#include "util/async.h"
#include "util/overloaded.h"
#include "util.h"

namespace ouinet {

using ouinet::bittorrent::NodeID;
using Error = I2pTrackerClient::Error;

static std::string percent_encode(const boost::string_view in) {
    if (in.empty()) return {};
    namespace urls = boost::urls;
    auto ev = urls::encode(in, urls::unreserved_chars);
    return std::string(ev.begin(), ev.end());
}

static void fill_common_target(std::ostream& os, const NodeID& infohash, const I2pAddress& local_server_addr) {
    // As we only use bittorent protocol to announce and we do not use it for downloading the cached
    // content we opportunistically re-randomize the peer_id on each announce least we leave 
    // unnecessary tracking traces behind.
    auto peer_id = NodeID::random();

    // Clients generally include a fake port=6881 parameter in the announce,
    // for compatibility with older trackers. Trackers may ignore the port
    // parameter, and should not require it.
    // "/a" is the announce path used by I2P trackers (zzzot/opentracker) to
    // save on length.  The ip parameter is the base 64 of the client’s
    // Destination.  Clients generally append “.i2p” to the Base 64 Destination
    // for compatibility with older trackers.
    os << "/a"
       << "?info_hash=" << percent_encode(infohash.to_bytestring())
       << "&peer_id=" << percent_encode(peer_id.to_bytestring())
       << "&ip=" << percent_encode(local_server_addr.value);
}

static std::string get_peers_target(const NodeID& infohash, const I2pAddress& local_server_addr) {
    std::stringstream s;
    fill_common_target(s, infohash, local_server_addr);
    s << "&compact=1"
      << "&numwant=50";
    return s.str();
}

static std::string announce_target(const NodeID& infohash, const I2pAddress& local_server_addr) {
    std::stringstream s;
    fill_common_target(s, infohash, local_server_addr);
    s << "&compact=1"
      << "&numwant=0";
    return s.str();
}

// Handshake probe: dummy all-zero infohash + event=stopped, so we don't
// pollute the tracker while still forcing a full round-trip through I2P and
// therefore lease-set exchange / tunnel build.
static std::string handshake_target(const I2pAddress& local_server_addr) {
    NodeID dummy_infohash{NodeID::Buffer{}};
    std::stringstream s;
    fill_common_target(s, dummy_infohash, local_server_addr);
    s << "&compact=1"
      << "&numwant=0"
      << "&event=stopped";
    return s.str();
}

static http::request<http::empty_body> make_request(const I2pAddress& tracker_addr, std::string target) {
    http::request<http::empty_body> rq{http::verb::get, std::move(target), 11};
    rq.set(http::field::host, tracker_addr.value);
    rq.set(http::field::user_agent, "Ouinet/1.0");
    return rq;
}

std::expected<std::string, Error::SendRequest>
I2pTrackerClient::send_request(const std::string& target, Async yield)
{
    static auto error = [] (auto e) { return std::unexpected<Error::SendRequest>(std::move(e)); };

    auto socket = _session->connect(_tracker_addr, yield);
    if (!socket) return error(std::move(socket.error()));

    auto request = make_request(_tracker_addr, target);

    auto slot = yield.cancel_slot([&] { if (socket->is_open()) socket->close(); });

    auto wr = http::async_write(*socket, request, yield);
    if (!wr) return error(Error::HttpSend{wr.error()});

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    auto rr = http::async_read(*socket, buffer, res, yield);
    if (!wr) return error(Error::HttpRecv{rr.error()});

    if (res.result() != http::status::ok) {
        return error(Error::HttpResult{res.result()});
    }

    return res.body();
}

std::expected<void, Error::Announce>
I2pTrackerClient::announce(NodeID infohash, Async yield)
{
    auto r = send_request(announce_target(infohash, _session->local_addr()), yield);
    if (!r) return std::unexpected<Error::Announce>(std::move(r.error()));
    LOG_DEBUG("BEP3 tracker: announce successful for ", infohash);
    return std::expected<void, Error::Announce>();
}

std::expected<void, Error::Announce>
I2pTrackerClient::handshake(Async yield)
{
    // Log our serving identity in the exact form the integration tests key on
    // (b32=<52 chars>.b32.i2p): they use it to correlate this client's b32
    // with the peers the tracker returns on another client's get_peers.
    LOG_DEBUG("BEP3 tracker: serving identity b32=", _session->local_addr());

    for (uint32_t i = 0;; ++i) {
        auto r = send_request(handshake_target(_session->local_addr()), yield);
        if (r) {
            LOG_DEBUG("BEP3 tracker: tracker handshake successful");
            return std::expected<void, Error::Announce>();
        }
        LOG_DEBUG("BEP3 tracker: handshake attempt failed; ", r.error());
        util::exponential_backoff(i, yield);
    }
}

std::expected<std::set<I2pAddress>, Error::GetPeers>
I2pTrackerClient::get_peers(NodeID infohash, Async yield)
{
    static auto error = [] (auto e) { return std::unexpected<Error::GetPeers>(std::move(e)); };

    auto body = send_request(get_peers_target(infohash, _session->local_addr()), yield);

    if (!body) return error(std::move(body.error()));

    // Parse bencoded tracker response
    auto decoded = bittorrent::bencoding_decode(*body);
    if (!decoded || !decoded->is_map()) {
        LOG_WARN("BEP3 tracker: not a bencoded body\n", *body);
        return error(Error::InvalidResponse { std::move(*body) });
    }

    auto* map = decoded->as_map();
    auto peers_it = map->find("peers");
    if (peers_it == map->end()) {
        LOG_WARN("BEP3 tracker: no peers key in response");
        return error(Error::InvalidResponse { std::move(*body) });
    }

    std::set<I2pAddress> peers;

    if (peers_it->second.is_list()) {
        // Non-compact format: list of dicts with "ip", "port", "peer id"
        // Zzzot never responds with this, so currently this never runs.
        // but maybe other trackers conform to compact=0
        for (const auto& peer_val : *peers_it->second.as_list()) {
            if (!peer_val.is_map()) continue;
            auto* pm = peer_val.as_map();
            auto ip_it = pm->find("ip");
            if (ip_it == pm->end()) continue;
            auto ip = ip_it->second.as_string();
            if (!ip || ip->empty()) continue;
            auto addr = I2pAddress::parse(*ip);
            if (!addr) continue;
            LOG_DEBUG("BEP3 tracker: found peer dest: ", *addr);
            peers.insert(std::move(*addr));
        }
    } else if (peers_it->second.is_string()) {
        // Compact I2P format: concatenated 32-byte DestHashes
        const auto data = *peers_it->second.as_string();
        constexpr size_t SIZE = I2pAddress::B32_ADDR_BINARY_SIZE;

        for (size_t i = 0; i + SIZE <= data.size() ; i += SIZE) {
            auto dest = *I2pAddress::from_binary_b32(std::span((unsigned char*)data.data() + i, SIZE));
            LOG_DEBUG("BEP3 tracker: found peer dest: ", dest);
            peers.insert(std::move(dest));
        }
    }

    LOG_DEBUG("BEP3 tracker: found ", peers.size(), " peers for ", infohash);

    return peers;
}

} // namespace
