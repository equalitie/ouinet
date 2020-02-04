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
    LocalPeerDiscovery(const asio::executor&, std::set<udp::endpoint> advertised_eps);

    LocalPeerDiscovery(const LocalPeerDiscovery&) = delete;

    std::set<udp::endpoint> found_peers() const;

    ~LocalPeerDiscovery();

    void stop();

private:
    asio::executor _ex;
    std::unique_ptr<Impl> _impl;
    Cancel _lifetime_cancel;
};

} // namespace
