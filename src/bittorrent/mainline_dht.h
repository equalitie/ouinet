#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include <set>
#include <map>

#include <asio_utp/udp_multiplexer.hpp>

#include "bootstrap.h"
#include "mutable_data.h"
#include "node_id.h"
#include "cxx/metrics.h"
#include "dht.h"

#include "../util/executor.h"
#include "../namespaces.h"
#include "../util/signal.h"

namespace ouinet::bittorrent {

class UdpMultiplexer;
class DhtNode;

namespace ip = asio::ip;
using ip::udp;
using util::AsioExecutor;

class MainlineDht : public DhtBase {
    public:
    MainlineDht( const AsioExecutor&
               , metrics::MainlineDht
               , boost::filesystem::path storage_dir = {}
               , std::set<bootstrap::Address> extra_bs = {});

    MainlineDht(const MainlineDht&) = delete;
    MainlineDht& operator=(const MainlineDht&) = delete;

    ~MainlineDht();

    // This removes existing endpoints not in the given set.
    // Since adding some endpoints may fail (e.g. because of port busy),
    // you may want to check `local_endpoints()` after this operation.
    void set_endpoints(const std::set<udp::endpoint>&) override;

    void add_endpoint(asio_utp::udp_multiplexer);

    udp::endpoint add_endpoint(asio_utp::udp_multiplexer, asio::yield_context) override;

    std::set<udp::endpoint> local_endpoints() const override {
        std::set<udp::endpoint> ret;
        for (auto& p : _nodes) { ret.insert(p.first); }
        return ret;
    }

    std::set<udp::endpoint> wan_endpoints() const override;

    /*
     * TODO: announce() and put() functions don't have any real error detection.
     */
    std::set<udp::endpoint> tracker_announce(
        NodeID infohash,
        boost::optional<int> port,
        Cancel,
        asio::yield_context
    ) override;

    void mutable_put(const MutableDataItem&, Cancel&, asio::yield_context);

    std::set<udp::endpoint> tracker_get_peers(NodeID infohash, Cancel&, asio::yield_context) override;

    boost::optional<BencodedValue> immutable_get(NodeID key, Cancel&, asio::yield_context);

    /*
     * TODO:
     *
     * Ideally, this interface should provide some way for the user to signal
     * when the best result found so far is good (that is, recent) enough, and
     * when to keep searching in the hopes of finding a more recent entry.
     * The current version is a quick-and-dirty good-enough-for-now.
     */
    boost::optional<MutableDataItem> mutable_get(
        const util::Ed25519PublicKey& public_key,
        boost::string_view salt,
        Cancel&,
        asio::yield_context
    );

    AsioExecutor get_executor() override { return _exec; }

    bool all_ready() const override;

    bool is_bootstrapped() const override {
        return !local_endpoints().empty() && all_ready();
    }

    void wait_all_ready(Cancel&, asio::yield_context) override;

    void stop() override;

    private:
    AsioExecutor _exec;
    std::map<udp::endpoint, std::unique_ptr<DhtNode>> _nodes;
    Cancel _cancel;
    boost::filesystem::path _storage_dir;
    std::set<bootstrap::Address> _extra_bs;
    metrics::MainlineDht _metrics;
};

} // namespace ouinet::bittorrent
