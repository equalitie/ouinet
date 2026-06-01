#pragma once

#include "address.h"
#include "tracker.h"
#include "declspec.h"

namespace ouinet {

class Async;
class I2pTrackerClient;

namespace bittorrent { class NodeID; }

class I2pTrackerLookup {
private:
    struct Inner;

public:
    using Error = I2pTrackerClient::Error::GetPeers;

    I2pTrackerLookup(std::shared_ptr<I2pTrackerClient>, const bittorrent::NodeID&);

    std::expected<std::set<I2pAddress>, Error> get(Async);

    const bittorrent::NodeID& infohash() const;

private:
    std::shared_ptr<Inner> _inner;
};

} // namespace
