#pragma once

#include "../../namespaces.h"
#include "../../util/signal.h"
#include <boost/asio/ip/udp.hpp>
#include <set>

namespace ouinet {

class LocalPeerDiscovery {
    using udp = asio::ip::udp;
    struct Impl;

public:
    LocalPeerDiscovery(asio::io_context&, std::set<udp::endpoint> advertised_eps);

    LocalPeerDiscovery(const LocalPeerDiscovery&) = delete;

    std::set<udp::endpoint> found_peers() const;

    ~LocalPeerDiscovery();

private:
    asio::io_context& _ctx;
    std::unique_ptr<Impl> _impl;
    Cancel _lifetime_cancel;
};

} // namespace
