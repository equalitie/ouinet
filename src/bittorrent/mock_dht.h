#pragma once

#include <map>
#include "asio_utp/udp_multiplexer.hpp"
#include "dht.h"
#include "api.h"
#include "udp_sockets.h"

namespace ouinet::bittorrent {

class OUINET_COMMON_API MockDht : public DhtBase {
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

    Promise<UdpEndpoint>::Future add_endpoint(asio_utp::udp_multiplexer) override;

    std::set<UdpEndpoint> local_endpoints() const override;

    std::set<UdpEndpoint> wan_endpoints() const override;

    UdpSockets sockets() const override {
        return UdpSockets(_sockets);
    }

    std::expected<std::set<UdpEndpoint>, sys::error_code>
    tracker_announce(NodeID infohash, std::optional<int> port, Async) override;

    std::expected<std::set<UdpEndpoint>, sys::error_code>
    tracker_get_peers(NodeID infohash, Async) override;

    Executor get_executor() override;

    bool all_ready() const override;

    bool is_bootstrapped() const override;

    void wait_all_ready(Async) override;

    void stop() override;

    // Normal DHT wouldn't allow localhost endpoints.
    bool is_peer_allowed(const UdpEndpoint&) const override { return true; }

    void can_not_see(std::string peer_name);

private:
    // Useful for debugging and to restrict access in tests (see _no_see_filter below)
    std::string _name;
    Executor _exec;
    std::shared_ptr<Swarms> _swarms;
    std::vector<asio_utp::udp_multiplexer> _sockets;
    // This peer won't find other peers with names in this filter
    std::set<std::string> _no_see_filter;
};

} // namespace ouinet::bittorrent
