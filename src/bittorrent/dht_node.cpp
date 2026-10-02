#include "dht_node.h"
#include "udp_multiplexer.h"

namespace ouinet::bittorrent {

asio_utp::udp_multiplexer DhtNode::udp_multiplexer() const {
    assert(_multiplexer);
    return _multiplexer->inner_udp_multiplexer();
}


} // namespace
