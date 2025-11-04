#pragma once

#include "dht.h"
#include <map>

namespace ouinet::bittorrent {

class MockDht : public DhtBase {
public:
    class Swarms : public std::map<NodeID, std::set<UdpEndpoint>> {};

public:
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

private:
    // Useful for debugging and later to restrict access in tests
    std::string _name;
    Executor _exec;
    std::shared_ptr<Swarms> _swarms;
    std::set<UdpEndpoint> _local_endpoints;
};

} // namespace ouinet::bittorrent
