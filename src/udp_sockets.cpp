#include <asio_utp/udp_multiplexer.hpp>
#include "create_udp_multiplexer.h"
#include "udp_sockets.h"

namespace ouinet {

using boost::asio::ip::udp;

UdpSockets UdpSockets::create(
    const util::AsioExecutor& exec,
    const boost::filesystem::path& last_used_port_path,
    const std::optional<uint16_t> settings_port
) {
    std::vector<asio_utp::udp_multiplexer> sockets;
    sockets.reserve(1);
    sockets.push_back(create_udp_multiplexer(
        exec,
        last_used_port_path,
        settings_port
    ));

    return UdpSockets(std::move(sockets));
}

std::set<udp::endpoint> UdpSockets::local_endpoints() const {
    std::set<udp::endpoint> eps;
    for (const auto& socket : _sockets) {
        eps.insert(socket.local_endpoint());
    }

    return eps;
}

} // namespace ouinet
