#include "peer_filter.h"
#include "is_martian.h"

namespace ouinet::bittorrent {

const PeerFilter PeerFilter::none(PeerFilter::Kind::none);
const PeerFilter PeerFilter::martian(PeerFilter::Kind::martian);

bool PeerFilter::is_allowed(const boost::asio::ip::udp::endpoint& ep) const {
    switch (_kind) {
        case Kind::martian:
            return !is_martian(ep);
        case Kind::none:
        default:
            return true;
    }
}

} // namespace ouinet::bittorrent
