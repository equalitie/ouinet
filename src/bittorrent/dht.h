#pragma once

#include <asio_utp/udp_multiplexer.hpp>
#include <set>
#include "node_id.h"
#include "namespaces.h"
#include "../util/promise.h"

namespace ouinet {

class Cancel;

namespace bittorrent {

class DhtBase {
public:
    using UdpEndpoint = asio::ip::udp::endpoint;
    using Executor = boost::asio::any_io_executor;

    DhtBase();
    DhtBase(const DhtBase&) = delete;
    DhtBase& operator=(const DhtBase&) = delete;

    virtual ~DhtBase();

    virtual Promise<UdpEndpoint>::Future add_endpoint(asio_utp::udp_multiplexer) = 0;

    virtual std::set<UdpEndpoint> local_endpoints() const = 0;

    virtual std::set<UdpEndpoint> wan_endpoints() const = 0;

    virtual std::vector<asio_utp::udp_multiplexer> sockets() const = 0;

    virtual std::expected<std::set<UdpEndpoint>, sys::error_code>
    tracker_announce(NodeID infohash, std::optional<int> port, Async) = 0;

    virtual std::expected<std::set<UdpEndpoint>, sys::error_code>
    tracker_get_peers(NodeID infohash, Async) = 0;

    virtual Executor get_executor() = 0;

    virtual bool all_ready() const = 0;

    virtual bool is_bootstrapped() const = 0;

    virtual void wait_all_ready(Async) = 0;

    virtual void stop() = 0;

    virtual bool is_peer_allowed(const UdpEndpoint&) const = 0;
};

}} // namespace ouinet::bittorrent
