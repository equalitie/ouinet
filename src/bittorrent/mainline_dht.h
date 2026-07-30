#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include <set>
#include <map>

#include <asio_utp/udp_multiplexer.hpp>

#include "api.h"
#include "bootstrap.h"
#include "cxx/dns.h"
#include "cxx/metrics.h"
#include "dht.h"
#include "mutable_data.h"
#include "node_id.h"
#include "peer_filter.h"


#include "../util/condition_variable.h"
#include "../util/executor.h"
#include "../util/log_path.h"
#include "../namespaces.h"

namespace ouinet {
    class Cancel;
}

namespace ouinet::bittorrent {

class UdpMultiplexer;
class DhtNode;

namespace ip = asio::ip;
using ip::udp;
using util::AsioExecutor;

// TODO: This is exposed here in this header only because it's also used in tests.
OUINET_COMMON_API
std::expected<asio::ip::udp::endpoint, sys::error_code> resolve(
    asio::ip::udp ipv,
    const std::string& addr,
    const std::string& port,
    const std::shared_ptr<dns::Resolver>& dns_resolver,
    Async);

class OUINET_COMMON_API MainlineDht : public DhtBase {
    public:
    MainlineDht( const AsioExecutor&
               , metrics::MainlineDht
               , std::shared_ptr<dns::Resolver>
               , uint32_t mux_rx_limit
               , boost::filesystem::path storage_dir
               , bootstrap::Config bs
               , util::LogPath);

    MainlineDht(const MainlineDht&) = delete;
    MainlineDht& operator=(const MainlineDht&) = delete;

    ~MainlineDht();

    // This removes existing endpoints not in the given set.
    // Since adding some endpoints may fail (e.g. because of port busy),
    // you may want to check `local_endpoints()` after this operation.
    void set_endpoints(const std::set<udp::endpoint>&) override;

    [[nodiscard]]
    Promise<udp::endpoint>::Future add_endpoint(asio_utp::udp_multiplexer) override;

    std::set<udp::endpoint> local_endpoints() const override {
        std::set<udp::endpoint> ret;
        for (auto& p : _nodes) { ret.insert(p.first); }
        return ret;
    }

    std::set<udp::endpoint> wan_endpoints() const override;

    std::expected<std::set<udp::endpoint>, sys::error_code>
    tracker_announce(NodeID infohash, std::optional<int> port, Async) override;

    std::expected<std::set<udp::endpoint>, sys::error_code>
    tracker_get_peers(NodeID infohash, Async) override;

    boost::optional<BencodedValue> immutable_get(NodeID key, Cancel&, asio::yield_context);

    void mutable_put(const MutableDataItem&, Cancel&, asio::yield_context);

    /*
     * TODO:
     *
     * Ideally, this interface should provide some way for the user to signal
     * when the best result found so far is good (that is, recent) enough, and
     * when to keep searching in the hopes of finding a more recent entry.
     * The current version is a quick-and-dirty good-enough-for-now.
     */
    boost::optional<MutableDataItem> mutable_get(
        const sign::PublicKey& public_key,
        boost::string_view salt,
        Cancel&,
        asio::yield_context
    );

    AsioExecutor get_executor() override { return _exec; }

    bool all_ready() const override;

    bool is_bootstrapped() const override {
        return !local_endpoints().empty() && all_ready();
    }

    void wait_all_ready(Async) override;

    void stop() override;

    bool is_peer_allowed(const UdpEndpoint&) const override;
    void set_peer_filter(PeerFilter);

    private:
    AsioExecutor _exec;
    std::map<udp::endpoint, std::unique_ptr<DhtNode>> _nodes;
    ConditionVariable _ready_cv; // notified every time a node becomes ready
    Cancel _cancel;
    std::shared_ptr<dns::Resolver> _dns_resolver;
    uint32_t _mux_rx_limit;
    boost::filesystem::path _storage_dir;
    bootstrap::Config _bootstrap_config;
    PeerFilter _peer_filter = PeerFilter::martian;
    metrics::MainlineDht _metrics;
    util::LogPath _log_path;
};

} // namespace ouinet::bittorrent
