#include "tracker_lookup.h"
#include "bittorrent/node_id.h"
#include "util/wait_condition.h"
#include "util/overloaded.h"
#include "util/async.h"

namespace ouinet {

using bittorrent::NodeID;
using Error = I2pTrackerLookup::Error;
using Result = std::expected<std::set<I2pAddress>, Error>;

using State = std::variant<
    // No work started yet
    std::monostate,
    // Work started
    WaitCondition,
    // Work finished
    Result
>;

struct I2pTrackerLookup::Inner {
    std::shared_ptr<I2pTrackerClient> tracker;
    NodeID infohash;
    State state;

    Inner(std::shared_ptr<I2pTrackerClient> tracker, const NodeID& infohash):
        tracker(std::move(tracker)),
        infohash(infohash),
        state(std::monostate())
    {}

    Result get(Async yield) {
        return std::visit(overloaded {
            [&] (std::monostate) {
                WaitCondition wc(yield.get_executor());
                auto lock = wc.lock();
                state = std::move(wc);
                auto result = tracker->get_peers(infohash, yield);
                state = result;
                return result;
            },
            [&] (WaitCondition& wc) {
                wc.wait(yield);
                auto result = std::get_if<Result>(&state);
                assert(result);
                return *result;
            },
            [&] (std::expected<std::set<I2pAddress>, Error>& result) {
                return result;
            }
        },
        state);
    }
};

I2pTrackerLookup::I2pTrackerLookup(std::shared_ptr<I2pTrackerClient> tracker, const bittorrent::NodeID& infohash):
    _inner(std::make_shared<Inner>(std::move(tracker), infohash))
{}

Result I2pTrackerLookup::get(Async yield) {
    return _inner->get(yield);
}

const NodeID& I2pTrackerLookup::infohash() const {
    return _inner->infohash;
}

} // namespace
