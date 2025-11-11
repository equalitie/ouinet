#pragma once

#include <memory>
#include <string>
#include <ostream>

namespace ouinet::util {

// To help keep track about which coroutine we're logging from.
class LogTree {
public:
    class Node {
        std::string _tag;
        std::shared_ptr<Node> _parent;

        friend class LogTree;

    public:
        Node(std::string tag, std::shared_ptr<Node> parent);
    };

    LogTree() = default;
    LogTree(std::string tag);

    LogTree(const LogTree&) = default;
    LogTree(LogTree&&) = default;
    LogTree& operator=(LogTree&&) = default;

    // Create new log tree with a new node having parent the one from `this`.
    LogTree tag(std::string tag);

    friend std::ostream& operator<<(std::ostream& os, LogTree const& l) {
        print_from_root(os, l._node);
        return os;
    }

private:
    LogTree(std::shared_ptr<Node> node) : _node(std::move(node)) {}

    static
    bool print_from_root(std::ostream&, const std::shared_ptr<LogTree::Node>&);

private:
    std::shared_ptr<Node> _node;
};

} // namespace
