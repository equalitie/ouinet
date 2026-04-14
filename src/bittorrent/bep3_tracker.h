#pragma once

#include <set>
#include <string>
#include <memory>
#include <boost/asio/spawn.hpp>
#include "node_id.h"
#include "namespaces.h"
#include "util/signal.h"
#include "util/wait_condition.h"
#include "ouiservice/i2p/fwd.h"

namespace ouinet::bittorrent {

// Encapsulates the BEP3 HTTP tracker protocol over I2P,
// analogous to how DhtBase encapsulates the DHT protocol.
class Bep3Tracker {
public:
    Bep3Tracker(I2pServer const&, std::string tracker_id);

    ~Bep3Tracker();

    void tracker_announce(NodeID infohash, Cancel&, asio::yield_context);

    std::set<std::string> tracker_get_peers(NodeID infohash, Cancel&, asio::yield_context);

    asio::any_io_executor get_executor();

private:
    // Send an HTTP GET request to the tracker per the BEP 3 tracker protocol
    // (https://www.bittorrent.org/beps/bep_0003.html) with I2P amendments
    // (https://geti2p.net/en/docs/applications/bittorrent):
    // the ip field carries the Base64 I2P Destination instead of an IP address.
    // Builds common query parameters and appends extra_params,
    // returns the response body.
    std::string send_request(const std::string& extra_params, Cancel&, asio::yield_context);

    // Wait for the I2P tunnel to start
    void ensure_started(Cancel, asio::yield_context);

    static NodeID generate_random_peer_id();

    static constexpr const char* _peer_id_prefix = "-OU0001-";
    static constexpr size_t _peer_id_prefix_len = 8;

    Cancel _cancel;
    std::shared_ptr<I2pClientDestination> _destination;
    std::shared_ptr<I2pClient> _i2p_client;
    std::string _serving_i2p_id;

    using StartState = std::variant<std::shared_ptr<WaitCondition>, sys::error_code>;
    std::shared_ptr<StartState> _start_state;
};

} // namespaces
