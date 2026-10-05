#pragma once

#include <namespaces.h>
#include <util/executor.h>
#include <util/cancel.h>
#include <boost/asio/ip/udp.hpp>
#include <set>
#include <vector>

namespace asio_utp { class udp_multiplexer; }

namespace ouinet {

using ouinet::util::AsioExecutor;

class LocalPeerDiscovery {
    using udp = asio::ip::udp;
    struct Impl;

public:
    LocalPeerDiscovery(const AsioExecutor&, std::vector<asio_utp::udp_multiplexer>);

    LocalPeerDiscovery(const LocalPeerDiscovery&) = delete;

    std::set<udp::endpoint> found_peers() const;

    ~LocalPeerDiscovery();

    void stop();

private:
    AsioExecutor _ex;
    std::unique_ptr<Impl> _impl;
    Cancel _lifetime_cancel;
};

} // namespace
