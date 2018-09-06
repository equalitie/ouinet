#pragma once

#include "routing_table.h" // For NodeContact

namespace ouinet { namespace bittorrent {

struct Contact {
    asio::ip::udp::endpoint endpoint;
    boost::optional<NodeID> id;

    Contact(asio::ip::udp::endpoint ep, boost::optional<NodeID> id)
        : endpoint(ep)
        , id(id)
    {}

    Contact(const dht::NodeContact& c)
        : endpoint(c.endpoint)
        , id(c.id)
    {}
};

}} // namespaces
