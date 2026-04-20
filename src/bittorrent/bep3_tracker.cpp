#ifdef __EXPERIMENTAL__

#include <sstream>
#include <cstdlib>
#include <ctime>

#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include "bencoding.h"
#include "bep3_tracker.h"
#include "ouiservice/i2p.h"
#include "../util.h"
#include "../logger.h"
#include "util/overloaded.h"
#include <ouiservice/i2p/address.h>
#include <ouiservice/i2p/i2pd/libi2pd/Base.h>  // ByteStreamToBase32

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;

namespace http = boost::beast::http;
namespace beast = boost::beast;

Bep3Tracker::Bep3Tracker(I2pServer const& i2p_server, I2pAddress tracker_id)
    : _destination(i2p_server.get_destination())
    , _i2p_client(i2p_server.get_service()->build_client(tracker_id, _destination))
    , _start_state(std::make_shared<StartState>(std::make_shared<WaitCondition>(_i2p_client->get_executor())))
{
    assert(_destination && "Bep3Tracker requires a valid destination");
    _serving_i2p_id = _destination->GetIdentity()->ToBase64(); //We are sure _destination isn't null Q.E.D

    task::spawn_detached(_i2p_client->get_executor(),
        [ i2p_client = _i2p_client
        , start_state = _start_state
        , lock = std::get<std::shared_ptr<WaitCondition>>(*_start_state)->lock()
        ] (auto yield) {
            sys::error_code ec;
            // TODO: Cancel
            i2p_client->start(yield[ec]);
            *start_state = ec;
        });
}

Bep3Tracker::~Bep3Tracker()
{
    _cancel();
}

void Bep3Tracker::ensure_started(Cancel cancel, asio::yield_context yield)
{
    auto slot = _cancel.connect([&] { cancel(); });

    std::visit(overloaded {
        [&] (std::shared_ptr<WaitCondition> wc) {
            sys::error_code ignored_ec;
            wc->wait(cancel, yield[ignored_ec]);
            if (cancel) return or_throw(yield, asio::error::operation_aborted);
            return or_throw(yield, std::get<sys::error_code>(*_start_state));
        },
        [&] (sys::error_code ec) {
            return or_throw(yield, ec);
        }
    },
    *_start_state);
}

asio::any_io_executor Bep3Tracker::get_executor()
{
    return _i2p_client->get_executor();
}

string Bep3Tracker::send_request( const string& extra_params
                                , Cancel& cancel
                                , asio::yield_context yield)
{
    auto slot = _cancel.connect([&] { cancel(); });
    sys::error_code ec;

    // Start the I2P tunnel if not already started, no-op otherwise
    ensure_started(cancel, yield[ec]);
    // Ensure that operation_aborted take precedence over ec.
    ec = compute_error_code(ec, cancel);
    if (ec) return or_throw(yield, ec, string{});

    //tracker follows standard http protocol doesn't conform to our handshake
    auto stream = _i2p_client->connect_without_handshake(yield[ec], cancel);
    ec = compute_error_code(ec, cancel);
    if (ec) return or_throw(yield, ec, string{});

    // As we only use bittorent protocol to announce and we do not use it for downloading the cached
    // content we opportunistically re-randomize the peer_id on each announce least we leave 
    // unnecessary tracking traces behind.
    auto peer_id = NodeID::random();

    string peer_id_encoded = util::percent_encode(peer_id.to_bytestring());

    ///Clients generally include a fake port=6881 parameter in the announce, for compatibility with older trackers. Trackers may ignore the port parameter, and should not require it.
    ostringstream target;
    // "/a" is the announce path used by I2P trackers (zzzot/opentracker) to save on length.
    // The ip parameter is the base 64 of the client’s Destination.
    // Clients generally append “.i2p” to the Base 64 Destination for compatibility with older trackers.
    target << "/a"
           << "?peer_id=" << peer_id_encoded
           << "&port=6881" //this is just ignored in i2p over bittorrent
           << "&ip=" << util::percent_encode(_serving_i2p_id + ".i2p")
           << "&uploaded=0"
           << "&downloaded=0"
           << extra_params;

    http::request<http::empty_body> req{http::verb::get, target.str(), 11};
    req.set(http::field::host, _i2p_client->get_target_id().value);
    req.set(http::field::user_agent, "Ouinet/1.0");

    LOG_DEBUG("BEP3 tracker: sending request: ", req);

    auto cancelled = cancel.connect([&] { stream.close(); });

    http::async_write(stream, req, yield[ec]);
    if (cancel) {
        LOG_DEBUG("BEP3 tracker: send request cancelled");
        return or_throw(yield, asio::error::operation_aborted, string{});
    } else if (ec) {
        LOG_WARN("BEP3 tracker: failed to send request; ec=", ec);
        return or_throw(yield, ec, string{});
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::async_read(stream, buffer, res, yield[ec]);
    if (cancel) {
        LOG_DEBUG("BEP3 tracker: read response cancelled");
        return or_throw(yield, asio::error::operation_aborted, string{});
    } else if (ec) {
        LOG_WARN("BEP3 tracker: failed to read response; ec=", ec);
        return or_throw(yield, ec, string{});
    }

    LOG_DEBUG("BEP3 tracker: HTTP ", static_cast<int>(res.result()),
              " ", res.reason(), " body=", res.body());

    if (res.result() != http::status::ok) {
        LOG_WARN("BEP3 tracker: tracker returned status ",
                 static_cast<int>(res.result()));
        return or_throw(yield, asio::error::connection_refused, string{});
    }

    return res.body();
}

void Bep3Tracker::tracker_announce( NodeID infohash
                                  , Cancel& cancel
                                  , asio::yield_context yield)
{
    string info_hash_encoded = util::percent_encode(infohash.to_bytestring());

    ostringstream params;
    params << "&left=0"  // seeder: we have the content
           << "&info_hash=" << info_hash_encoded
           << "&compact=1"
           << "&event=started"
           << "&numwant=24";

    sys::error_code ec;
    send_request(params.str(), cancel, yield[ec]);

    return or_throw(yield, ec);
}

set<I2pAddress> Bep3Tracker::tracker_get_peers( NodeID infohash
                                              , Cancel& cancel
                                              , asio::yield_context yield)
{
    string info_hash_encoded = util::percent_encode(infohash.to_bytestring());

    ostringstream params;
    params << "&left=1"  // leecher: looking for peers
           << "&info_hash=" << info_hash_encoded
           << "&compact=0" //Zzzot somehow ignores this, it always returns 32byte ids
           << "&numwant=50";

    sys::error_code ec;
    auto body = send_request(params.str(), cancel, yield[ec]);

    if (ec) return or_throw(yield, ec, set<I2pAddress>{});

    // Parse bencoded tracker response
    auto decoded = bencoding_decode(body);
    if (!decoded || !decoded->is_map()) {
        LOG_WARN("BEP3 tracker: invalid bencoded response");
        return or_throw(yield, asio::error::invalid_argument, set<I2pAddress>{});
    }

    auto* map = decoded->as_map();
    auto peers_it = map->find("peers");
    if (peers_it == map->end()) {
        LOG_WARN("BEP3 tracker: no peers key in response");
        return or_throw(yield, asio::error::invalid_argument, set<I2pAddress>{});
    }

    set<I2pAddress> peers;

    LOG_DEBUG("BEP3 tracker: peers type: ",
              peers_it->second.is_list() ? "list" :
              peers_it->second.is_string() ? "string" : "other",
              peers_it->second.is_string() ?
              " len=" + to_string(peers_it->second.as_string()->size()) : "");

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
            peers.insert(I2pAddress{*ip});
        }
    } else if (peers_it->second.is_string()) {
        // Compact I2P format: concatenated 32-byte DestHashes
        const auto& data = *peers_it->second.as_string();
        const size_t DEST_HASH_LEN = 32;
        for (size_t i = 0; i + DEST_HASH_LEN <= data.size(); i += DEST_HASH_LEN) {
            std::string dest = i2p::data::ByteStreamToBase32((const uint8_t*)data.data() + i, DEST_HASH_LEN);
            dest += ".b32.i2p";
            LOG_DEBUG("BEP3 tracker: found peer dest: ", dest);
            peers.insert(I2pAddress{move(dest)});
        }
    }

    LOG_DEBUG("BEP3 tracker: found ", peers.size(), " peers for ", infohash);

    return or_throw(yield, ec, move(peers));
}

#endif // __EXPERIMENTAL__
