#pragma once

#include <set>
#include <string>
#include <bittorrent/bep3_tracker.h>
#include "peer_lookup.h"
#include "ouiservice/i2p/address.h"

namespace ouinet::cache {

class Bep3TrackerLookup : public PeerLookup<std::set<I2pAddress>> {
public:
    Bep3TrackerLookup(Bep3TrackerLookup&&) = delete;

    Bep3TrackerLookup( std::shared_ptr<bittorrent::Bep3Tracker> tracker
                     , std::string swarm_name)
        : PeerLookup(std::move(swarm_name), tracker->get_executor())
        , _tracker(std::move(tracker))
    {
        _lookup_strategy_name = "BEP3 tracker";
    }

protected:
    Ret do_lookup(Cancel& c, asio::yield_context y) override {
        sys::error_code ec;
        auto peers = _tracker->tracker_get_peers(infohash(), c, y[ec]);
        return or_throw(y, ec, std::move(peers));
    }

private:
    std::shared_ptr<bittorrent::Bep3Tracker> _tracker;
};

} // namespaces
