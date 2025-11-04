#pragma once

#include "dht.h"
#include <map>

namespace ouinet::bittorrent {

class MockDht : public DhtBase {
    struct Peer {
        std::string name;
        UdpEndpoint endpoint;
        friend bool operator<(const Peer& p1, const Peer& p2) {
            return std::tie(p1.name, p1.endpoint) < std::tie(p2.name, p2.endpoint);
        }
    };

    struct Swarm : std::set<Peer> {
        std::set<UdpEndpoint> endpoints(const std::set<std::string>& no_see_filter);
    };

public:
    class Swarms : public std::map<NodeID, Swarm> {};

    MockDht(std::string name, Executor exec, std::shared_ptr<Swarms>);
    ~MockDht();

    void set_endpoints(const std::set<UdpEndpoint>&) override;

    UdpEndpoint add_endpoint(asio_utp::udp_multiplexer, asio::yield_context) override;

    std::set<UdpEndpoint> local_endpoints() const override;

    std::set<UdpEndpoint> wan_endpoints() const override;

    std::set<UdpEndpoint> tracker_announce(
        NodeID infohash,
        boost::optional<int> port,
        Cancel,
        asio::yield_context
    ) override;

    std::set<UdpEndpoint> tracker_get_peers(NodeID infohash, Cancel&, asio::yield_context) override;

    Executor get_executor() override;

    bool all_ready() const override;

    bool is_bootstrapped() const override;

    void wait_all_ready(Cancel&, asio::yield_context) override;

    void stop() override;

    // Normal DHT wouldn't allow localhost endpoints.
    bool is_martian(const UdpEndpoint&) const override { return false; }

    void can_not_see(std::string peer_name);

private:
    // Useful for debugging and to restrict access in tests (see _no_see_filter below)
    std::string _name;
    Executor _exec;
    std::shared_ptr<Swarms> _swarms;
    std::set<UdpEndpoint> _local_endpoints;
    // This peer won't find other peers with names in this filter
    std::set<std::string> _no_see_filter;
};

} // namespace ouinet::bittorrent
