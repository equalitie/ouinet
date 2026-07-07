#pragma once

#include <asio_utp/udp_multiplexer.hpp>
#include <set>
#include <type_traits>

#include "ouiservice/ouisync/ouisync.h"
#include "util/async.h"
#include "util/executor.h"

namespace ouinet {

static_assert(std::is_copy_constructible_v<asio_utp::udp_multiplexer>);

// Container for listening UDP sockets. Useful to conveniently pass the sockets to various
// components. Typically contains two sockets - one bound to IPv4 and one to IPv6 interface.
class UdpSockets {
public:
    using iterator = std::vector<asio_utp::udp_multiplexer>::iterator;
    using const_iterator = std::vector<asio_utp::udp_multiplexer>::const_iterator;

public:
    // Create standalone UDP sockets listening on specified ports.
    // Note currently this only creates a single IPv4 socket.
    static
    UdpSockets create(
        const util::AsioExecutor& exec,
        const boost::filesystem::path& last_used_port_path,
        const std::optional<uint16_t> settings_port
    );

    // Create UDP sockets backed by Ouisync.
    static
    std::expected<UdpSockets, boost::system::error_code>
    create(ouisync_service::Ouisync&, Async);

    UdpSockets() = default;

    UdpSockets(UdpSockets&&) = default;
    UdpSockets& operator=(UdpSockets&&) = default;

    UdpSockets(const UdpSockets&) = default;
    UdpSockets& operator=(const UdpSockets&) = default;

    explicit UdpSockets(std::vector<asio_utp::udp_multiplexer> sockets) :
        _sockets(std::move(sockets))
    {}

    bool empty() const {
        return _sockets.empty();
    }

    const_iterator begin() const {
        return _sockets.begin();
    }

    iterator begin() {
        return _sockets.begin();
    }

    const_iterator end() const {
        return _sockets.end();
    }

    iterator end() {
        return _sockets.end();
    }

    std::set<boost::asio::ip::udp::endpoint> local_endpoints() const;

private:
    std::vector<asio_utp::udp_multiplexer> _sockets;

};

} // namespace ouinet
