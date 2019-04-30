#pragma once

#include "node_id.h"
#include "code.h"

namespace ouinet { namespace bittorrent { namespace dht {

struct NodeContact {
    NodeID id;
    asio::ip::udp::endpoint endpoint;

    std::string to_string() const;

    bool operator==(const NodeContact& other) const {
        return id == other.id && endpoint == other.endpoint;
    }

    bool operator<(const NodeContact& other) const {
        return std::tie(id, endpoint) < std::tie(other.id, other.endpoint);
    }

    static
    boost::optional<NodeContact> decode_compact_v4(boost::string_view&);

    static
    boost::optional<NodeContact> decode_compact_v6(boost::string_view&);

    static
    bool decode_compact_v4(boost::string_view s, std::vector<NodeContact>&);

    static
    bool decode_compact_v6(boost::string_view s, std::vector<NodeContact>&);
};

inline
boost::optional<NodeContact>
NodeContact::decode_compact_v4(boost::string_view& s)
{
    if (s.size() < 26) return boost::none;

    return NodeContact {
        NodeID::from_bytestring(s.substr(0, 20)),
        *decode_endpoint(s.substr(20, 6))
    };

    s = s.substr(26);
}

inline
boost::optional<NodeContact>
NodeContact::decode_compact_v6(boost::string_view& s)
{
    if (s.size() < 38) return boost::none;

    return NodeContact {
        NodeID::from_bytestring(s.substr(0, 20)),
        *decode_endpoint(s.substr(20, 18))
    };

    s = s.substr(38);
}

inline
bool NodeContact::decode_compact_v4( boost::string_view str
                                   , std::vector<dht::NodeContact>& contacts)
{
    // 20 bytes of ID, plus 6 bytes of endpoint
    if (str.size() % 26) { return false; }

    for (unsigned int i = 0; i < str.size() / 26; i++) {
        auto s = str.substr(i * 26, 26);
        contacts.push_back(*decode_compact_v4(s));
    }

    return true;
}

inline
bool NodeContact::decode_compact_v6( boost::string_view str
                                   , std::vector<dht::NodeContact>& contacts)
{
    // 20 bytes of ID, plus 18 bytes of endpoint
    if (str.size() % 38) { return false; }

    for (unsigned int i = 0; i < str.size() / 38; i++) {
        auto s = str.substr(i * 38, 38);
        contacts.push_back(*decode_compact_v6(s));
    }

    return true;
}

inline
std::ostream& operator<<(std::ostream& os, const NodeContact& n)
{
    return os << "{" << n.id << ", " << n.endpoint << "}";
}

}}} // namespaces
