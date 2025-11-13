#pragma once

#include <boost/asio/spawn.hpp>
#include <set>
#include <asio_utp/udp_multiplexer.hpp>
#include "node_id.h"
#include "namespaces.h"
#include "util/signal.h"

namespace ouinet::bittorrent {

class DhtBase {
public:
    using UdpEndpoint = asio::ip::udp::endpoint;
    using Executor = boost::asio::any_io_executor;

    DhtBase();
    DhtBase(const DhtBase&) = delete;
    DhtBase& operator=(const DhtBase&) = delete;

    virtual ~DhtBase();

    virtual void set_endpoints(const std::set<UdpEndpoint>&) = 0;

    virtual UdpEndpoint add_endpoint(asio_utp::udp_multiplexer, asio::yield_context) = 0;

    virtual std::set<UdpEndpoint> local_endpoints() const = 0;

    virtual std::set<UdpEndpoint> wan_endpoints() const = 0;

    /*
     * TODO: announce() and put() functions don't have any real error detection.
     */
    virtual std::set<UdpEndpoint> tracker_announce(
        NodeID infohash,
        boost::optional<int> port,
        Cancel,
        asio::yield_context
    ) = 0;

    virtual std::set<UdpEndpoint> tracker_get_peers(NodeID infohash, Cancel&, asio::yield_context) = 0;

    virtual Executor get_executor() = 0;

    virtual bool all_ready() const = 0;

    virtual bool is_bootstrapped() const = 0;

    virtual void wait_all_ready(Cancel&, asio::yield_context) = 0;

    virtual void stop() = 0;

    virtual bool is_martian(const UdpEndpoint&) const = 0;
};

} // namespace ouinet::bittorrent
