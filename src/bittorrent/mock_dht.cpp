#include "mock_dht.h"
#include "udp_sockets.h"
#include "util/cancel.h"
#include "util/debug.h"

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

Promise<UdpEndpoint>::Future MockDht::add_endpoint(asio_utp::udp_multiplexer m) {
    auto ep = m.local_endpoint();
    std::cout << _name << ": add_endpoint to " << ep << "\n";

    _sockets.push_back(std::move(m));

    Promise<UdpEndpoint> promise(_exec);
    promise.set_value(ep);

    return promise.get_future();
}

std::set<UdpEndpoint> MockDht::local_endpoints() const {
    std::set<UdpEndpoint> eps;

    for (const auto& socket : _sockets) {
        eps.insert(socket.local_endpoint());
    }

    std::cout << _name << ": local_endpoints -> " << debug(eps) << "\n";

    return eps;
}

std::set<UdpEndpoint> MockDht::wan_endpoints() const {
    return {};
}

std::set<UdpEndpoint> MockDht::Swarm::endpoints(const std::set<std::string>& no_see_filter) {
    std::set<UdpEndpoint> eps;
    for (auto peer : *this) {
        if (no_see_filter.count(peer.name) == 0) {
            eps.insert(peer.endpoint);
        }
    }
    return eps;
}

/*
 * TODO: announce() and put() functions don't have any real error detection.
 */
std::expected<std::set<UdpEndpoint>, sys::error_code> MockDht::tracker_announce(
    NodeID infohash,
    std::optional<int> port,
    Async
) {
    std::set<UdpEndpoint> my_endpoints;

    for (const auto& socket : _sockets) {
        auto ep = socket.local_endpoint();

        if (port) {
            ep.port(*port);
        }
        my_endpoints.insert(any_to_local(ep));
    }

    std::cout << _name << ": announce " << debug(my_endpoints) << " to " << infohash << "\n";

    for (auto ep : my_endpoints) {
        (*_swarms)[infohash].insert(Peer { _name, ep });
    }

    return (*_swarms)[infohash].endpoints(_no_see_filter);
}

std::expected<std::set<UdpEndpoint>, sys::error_code>
MockDht::tracker_get_peers(NodeID infohash, Async) {
    auto swarm_i = _swarms->find(infohash);
    if (swarm_i == _swarms->end()) {
        std::cout << _name << ": get " << infohash << " -> {} (no such swarm)\n";
        return {};
    }

    auto eps = swarm_i->second.endpoints(_no_see_filter);

    std::cout << _name << ": get " << infohash << " -> " << debug(eps) << "\n";

    return eps;
}

void MockDht::can_not_see(std::string peer_name) {
    _no_see_filter.insert(peer_name);
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

void MockDht::wait_all_ready(Async) {
}

void MockDht::stop() {
}

} // namespace
