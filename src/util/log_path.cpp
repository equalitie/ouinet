#include "log_path.h"
#include "intrusive_list.h"

namespace ouinet::util {

struct RootData {
    std::ostream* monitor_changes = nullptr;
};

struct LogPath::Node {
    std::string _tag;
    std::shared_ptr<Node> _parent;
    std::shared_ptr<RootData> _root_data;

    intrusive::list_hook _hook;
    intrusive::list<Node, &Node::_hook> _children;

    Node(std::string tag, std::shared_ptr<Node> parent) :
        _tag(std::move(tag)),
        _parent(std::move(parent))
    {
        if (_parent) {
            _parent->_children.push_back(*this);
            _root_data = _parent->_root_data;
        } else {
            _root_data = std::make_shared<RootData>();
        }

        if (auto os = _root_data->monitor_changes) {
            *os << "+++ ";
            LogPath::print_from_root(*os, this);
            *os << "\n";
        }
    }

    ~Node() {
        if (auto os = _root_data->monitor_changes) {
            *os << "--- ";
            LogPath::print_from_root(*os, this);
            *os << "\n";
        }
    }
};

LogPath::LogPath(std::string tag) :
    _node(std::make_shared<Node>(std::move(tag), nullptr))
{}

LogPath LogPath::tag(std::string tag) {
    auto node = std::make_shared<Node>(std::move(tag), _node);
    return LogPath{std::move(node)};
}

void LogPath::print_from_root(std::ostream& os, const LogPath::Node* node) {
    if (node == nullptr) {
        return;
    }
    print_from_root(os, node->_parent.get());
    os << "/" << node->_tag;
}

void LogPath::start_monitor_changes(std::ostream& os) const {
    _node->_root_data->monitor_changes = &os;
}

} // namespace ouinet::util
