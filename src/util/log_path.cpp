#include "log_tree.h"

namespace ouinet::util {

LogTree::Node::Node(std::string tag, std::shared_ptr<Node> parent) :
    _tag(std::move(tag)),
    _parent(std::move(parent))
{}

LogTree::LogTree(std::string tag) :
    _node(std::make_shared<Node>(std::move(tag), nullptr))
{}

LogTree LogTree::tag(std::string tag) {
    auto node = std::make_shared<Node>(std::move(tag), _node);
    return LogTree{std::move(node)};
}

bool LogTree::print_from_root(std::ostream& os, const std::shared_ptr<LogTree::Node>& node) {
    if (node == nullptr) {
        return false;
    }
    if (print_from_root(os, node->_parent)) {
        os << "/";
    }
    os << node->_tag;
    return true;
}

} // namespace ouinet::util
