#include "dht.h"

namespace ouinet::bittorrent {

DhtBase::DhtBase() {}
DhtBase::~DhtBase() {}

std::set<DhtBase::UdpEndpoint> DhtBase::local_endpoints() const {
    std::set<UdpEndpoint> ret;
    for (auto& m : sockets()) {
        ret.insert(m.local_endpoint());
    }
    return ret;
}

} // namespace ouinet::bittorrent
