#include "routing_table.h"

using namespace ouinet::bittorrent::dht;

RoutingTable::RoutingTable(NodeID node_id) :
    _node_id(node_id)
{
    _root_node = std::make_unique<RoutingTreeNode>();
    _root_node->bucket = std::make_unique<RoutingBucket>();
}
