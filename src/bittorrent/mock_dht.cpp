#include "mock_dht.h"
#include "debug/set.h"

namespace ouinet::bittorrent {

using UdpEndpoint = MockDht::UdpEndpoint;
using Executor = MockDht::Executor;

static UdpEndpoint any_to_local(UdpEndpoint ep) {
    if (ep.address().is_unspecified()) {
        if (ep.address().is_v4()) {
            ep.address(asio::ip::address_v4::loopback());
        } else {
            ep.address(asio::ip::address_v6::loopback());
        }
    }
    return ep;
}

MockDht::MockDht(std::string name, Executor exec, std::shared_ptr<Swarms> swarms) :
    _name(std::move(name)),
    _exec(std::move(exec)),
    _swarms(std::move(swarms))
{
}

MockDht::~MockDht() {
}

void MockDht::set_endpoints(const std::set<UdpEndpoint>& eps) {
    std::cout << _name << ": set_endpoints to " << debug(eps) << "\n";
    _local_endpoints = eps;
}

UdpEndpoint MockDht::add_endpoint(asio_utp::udp_multiplexer m, asio::yield_context) {
    _local_endpoints.insert(m.local_endpoint());
    std::cout << _name << ": add_endpoint to " << m.local_endpoint() << "\n";
    return m.local_endpoint();
}

std::set<UdpEndpoint> MockDht::local_endpoints() const {
    std::cout << _name << ": local_endpoints -> " << debug(_local_endpoints) << "\n";
    return _local_endpoints;
}

std::set<UdpEndpoint> MockDht::wan_endpoints() const {
    return {};
}

/*
 * TODO: announce() and put() functions don't have any real error detection.
 */
std::set<UdpEndpoint> MockDht::tracker_announce(
    NodeID infohash,
    boost::optional<int> port,
    Cancel,
    asio::yield_context
) {
    std::set<UdpEndpoint> eps;
    for (auto ep : _local_endpoints) {
        if (port) {
            ep.port(*port);
        }
        eps.insert(any_to_local(ep));
    }

    std::cout << _name << ": announce " << debug(eps) << " to " << infohash << "\n";

    for (auto ep : eps) {
        (*_swarms)[infohash].insert(ep);
    }

    return (*_swarms)[infohash];
}

std::set<UdpEndpoint> MockDht::tracker_get_peers(NodeID infohash, Cancel&, asio::yield_context) {
    auto swarm_i = _swarms->find(infohash);
    if (swarm_i == _swarms->end()) {
        std::cout << _name << ": get " << infohash << " -> {} (no such swarm)\n";
        return {};
    }
    std::cout << _name << ": get " << infohash << " -> " << debug(swarm_i->second) << "\n";
    return swarm_i->second;
}

Executor MockDht::get_executor() {
    return _exec;
}

bool MockDht::all_ready() const {
    return true;
}

bool MockDht::is_bootstrapped() const {
    return true;
}

void MockDht::wait_all_ready(Cancel&, asio::yield_context) {
}

void MockDht::stop() {
}

} // namespace
