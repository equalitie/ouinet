#pragma once

#include <memory>
#include <string>
#include <ostream>
#include "api.h"

namespace ouinet {

// To help keep track about which coroutine we're logging from.
class OUINET_COMMON_API Trace {
private:
    struct Node;

public:
    Trace() = default;
    Trace(std::string tag);

    Trace(const Trace&) = default;
    Trace(Trace&&) = default;
    Trace& operator=(Trace&&) = default;

    // Create new log tree with a new node having parent the one from `this`.
    Trace tag(std::string tag);

    friend std::ostream& operator<<(std::ostream& os, Trace const& l) {
        print_from_root(os, l._node.get());
        return os;
    }

    void start_monitor_changes(std::ostream&) const;

private:
    Trace(std::shared_ptr<Node> node) : _node(std::move(node)) {}

    static
    void print_from_root(std::ostream&, const Node*);

private:
    std::shared_ptr<Node> _node;
};

} // namespace
