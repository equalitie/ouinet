#ifdef __EXPERIMENTAL__

#include <sstream>
#include <cstdlib>
#include <ctime>

#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include "bencoding.h"
#include "bep3_tracker.h"
#include <ouiservice/i2p/client.h>
#include <ouiservice/i2p/service.h>
#include "../util.h"
#include "../logger.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;

namespace http = boost::beast::http;
namespace beast = boost::beast;

Bep3Tracker::Bep3Tracker( shared_ptr<ouiservice::i2poui::Service> i2p_service
                         , string tracker_id
                         , string serving_i2p_id)
    : _i2p_client(i2p_service->build_client(tracker_id))
    , _serving_i2p_id(move(serving_i2p_id))
{ }

void Bep3Tracker::ensure_started(Cancel& cancel, asio::yield_context yield)
{
    if (_started) return;
    sys::error_code ec;
    _i2p_client->start(yield[ec]);
    if (!ec) _started = true;
    return or_throw(yield, ec);
}

Bep3Tracker::Executor Bep3Tracker::get_executor()
{
    return _i2p_client->get_executor();
}

NodeID Bep3Tracker::generate_random_peer_id()
{
    NodeID::Buffer buf;
    srand(time(nullptr));
    for (auto& b : buf) {
        b = rand() % 256;
    }
    const char* prefix = "-OU0001-";
    for (size_t i = 0; i < 8 && i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>(prefix[i]);
    }
    return NodeID(buf);
}

string Bep3Tracker::send_request( const string& extra_params
                                , Cancel& cancel
                                , asio::yield_context yield)
{
    sys::error_code ec;

    // Start the I2P tunnel if not already started, no-op otherwise
    ensure_started(cancel, yield[ec]);
    if (ec || cancel) {
        return or_throw(yield, ec ? ec : asio::error::operation_aborted, string{});
    }

    auto stream = _i2p_client->connect(yield[ec], cancel);
    if (ec || cancel) {
        return or_throw(yield, ec ? ec : asio::error::operation_aborted, string{});
    }

    auto peer_id = generate_random_peer_id();

    string peer_id_encoded = util::percent_encode(peer_id.to_bytestring());

    ///Clients generally include a fake port=6881 parameter in the announce, for compatibility with older trackers. Trackers may ignore the port parameter, and should not require it.
    ostringstream target;
    target << "/a"
           << "?peer_id=" << peer_id_encoded
           << "&port=6881" //this is just ignorred in i2p over bitttorent
           << "&ip=" << _serving_i2p_id << ".i2p"
           << "&uploaded=0"
           << "&downloaded=0"
           << "&left=1"
           << extra_params;

    http::request<http::empty_body> req{http::verb::get, target.str(), 11};
    req.set(http::field::host, _i2p_client->get_target_id());
    req.set(http::field::user_agent, "Ouinet/1.0");

    auto cancelled = cancel.connect([&] { stream.close(); });

    http::async_write(stream, req, yield[ec]);
    if (ec) {
        LOG_WARN("BEP3 tracker: failed to send request; ec=", ec);
        return or_throw(yield, ec, string{});
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::async_read(stream, buffer, res, yield[ec]);
    if (ec) {
        LOG_WARN("BEP3 tracker: failed to read response; ec=", ec);
        return or_throw(yield, ec, string{});
    }

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
    params << "&info_hash=" << info_hash_encoded
           << "&compact=1"
           << "&event=started"
           << "&numwant=24";

    sys::error_code ec;
    send_request(params.str(), cancel, yield[ec]);

    return or_throw(yield, ec);
}

set<string> Bep3Tracker::tracker_get_peers( NodeID infohash
                                          , Cancel& cancel
                                          , asio::yield_context yield)
{
    string info_hash_encoded = util::percent_encode(infohash.to_bytestring());

    ostringstream params;
    params << "&info_hash=" << info_hash_encoded
           << "&compact=0"
           << "&numwant=50";

    sys::error_code ec;
    auto body = send_request(params.str(), cancel, yield[ec]);

    if (ec) return or_throw(yield, ec, set<string>{});

    // Parse bencoded tracker response
    auto decoded = bencoding_decode(body);
    if (!decoded || !decoded->is_map()) {
        LOG_WARN("BEP3 tracker: invalid bencoded response");
        return or_throw(yield, asio::error::invalid_argument, set<string>{});
    }

    auto* map = decoded->as_map();
    auto peers_it = map->find("peers");
    if (peers_it == map->end()) {
        LOG_WARN("BEP3 tracker: no peers key in response");
        return or_throw(yield, asio::error::invalid_argument, set<string>{});
    }

    set<string> peers;

    if (peers_it->second.is_list()) {
        // Non-compact format: list of dicts with "ip", "port", "peer id"
        for (const auto& peer_val : *peers_it->second.as_list()) {
            if (!peer_val.is_map()) continue;
            auto* pm = peer_val.as_map();
            auto ip_it = pm->find("ip");
            if (ip_it == pm->end()) continue;
            auto ip = ip_it->second.as_string();
            if (!ip || ip->empty()) continue;
            peers.insert(*ip);
        }
    }

    LOG_DEBUG("BEP3 tracker: found ", peers.size(), " peers for ", infohash);

    return or_throw(yield, ec, move(peers));
}

#endif // __EXPERIMENTAL__
