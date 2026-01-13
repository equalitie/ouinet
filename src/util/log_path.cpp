#include "log_path.h"

namespace ouinet::util {

LogPath::Node::Node(std::string tag, std::shared_ptr<Node> parent) :
    _tag(std::move(tag)),
    _parent(std::move(parent))
{}

LogPath::LogPath(std::string tag) :
    _node(std::make_shared<Node>(std::move(tag), nullptr))
{}

LogPath LogPath::tag(std::string tag) {
    auto node = std::make_shared<Node>(std::move(tag), _node);
    return LogPath{std::move(node)};
}

void LogPath::print_from_root(std::ostream& os, const std::shared_ptr<LogPath::Node>& node) {
    if (node == nullptr) {
        return;
    }
    print_from_root(os, node->_parent);
    os << "/" << node->_tag;
}

} // namespace ouinet::util
