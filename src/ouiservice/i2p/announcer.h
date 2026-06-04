#pragma once

#include "api.h"
#include <memory>

namespace ouinet {

namespace bittorrent { class NodeID; }

class I2pTrackerClient;
class I2pAddress;

// Periodically announces our local I2pAddress to `add`ed info-hashes.
class I2pAnnouncer {
private:
    struct State;

public:
    I2pAnnouncer(std::shared_ptr<I2pTrackerClient>);

    bool add(const bittorrent::NodeID& infohash);

    bool remove(const bittorrent::NodeID& infohash);

    void close();

    // Destructor calls `close()` so enabling copy constructors would require
    // us to do reference counting (we can't use `_state`'s counter because
    // that one is shared with a spawned task).
    I2pAnnouncer(I2pAnnouncer const&) = delete;
    I2pAnnouncer(I2pAnnouncer &&) = default;
    I2pAnnouncer& operator=(I2pAnnouncer const&) = delete;
    I2pAnnouncer& operator=(I2pAnnouncer &&) = default;

    ~I2pAnnouncer();

private:
    std::shared_ptr<State> _state;
};

} // namespace
