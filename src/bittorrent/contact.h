#pragma once

namespace ouinet { namespace bittorrent {

struct Contact {
    asio::ip::udp::endpoint endpoint;
    boost::optional<NodeID> id;
};

}} // namespaces
