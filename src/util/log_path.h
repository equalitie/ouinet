#pragma once

#include <memory>
#include <string>
#include <ostream>

namespace ouinet::util {

// To help keep track about which coroutine we're logging from.
class LogPath {
private:
    struct Node;

public:
    LogPath() = default;
    LogPath(std::string tag);

    LogPath(const LogPath&) = default;
    LogPath(LogPath&&) = default;
    LogPath& operator=(LogPath&&) = default;

    // Create new log tree with a new node having parent the one from `this`.
    LogPath tag(std::string tag);

    friend std::ostream& operator<<(std::ostream& os, LogPath const& l) {
        print_from_root(os, l._node);
        return os;
    }

private:
    LogPath(std::shared_ptr<Node> node) : _node(std::move(node)) {}

    static
    void print_from_root(std::ostream&, const std::shared_ptr<Node>&);

private:
    std::shared_ptr<Node> _node;
};

} // namespace
