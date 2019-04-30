#pragma once

#include <boost/asio/ip/address.hpp>
#include <boost/optional.hpp>
#include <string>
#include <array>
#include "../namespaces.h"
#include "../util/bytes.h"

namespace ouinet { namespace bittorrent {

struct NodeID {
    static constexpr size_t size     = 20;
    static constexpr size_t bit_size = size * 8;

    using Buffer = std::array<uint8_t, size>;

    struct Range {
        Buffer stencil;
        size_t mask;

        NodeID random_id() const;
        Range reduce(bool bit) const;

        static const Range& max();
    };

    Buffer buffer;

    NodeID() = default;
    NodeID(const NodeID& other) : buffer(other.buffer) {}
    NodeID(const Buffer& buffer) : buffer(buffer) {}

    // XXX: `bit(0)` is the most signifficant, perhaps the function should be
    // called `rbit` ('r' for reverse)?
    bool bit(int n) const;
    void set_bit(int n, bool value);

    std::string to_hex() const { return util::bytes::to_hex(buffer); }

    static NodeID from_hex(boost::string_view hex) {
        return NodeID{ util::bytes::to_array<uint8_t, size>(util::bytes::from_hex(hex)) };
    }

    std::string to_printable() const { return util::bytes::to_printable(buffer); }

    static boost::optional<NodeID> from_printable(boost::string_view s) {
        auto a = util::bytes::from_printable(s);
        if (!a) return boost::none;
        return NodeID{util::bytes::to_array<uint8_t, size>(*a)};
    }

    std::string to_bytestring() const { return util::bytes::to_string(buffer); }

    static NodeID from_bytestring(boost::string_view bytestring) {
        return NodeID{ util::bytes::to_array<uint8_t, size>(bytestring) };
    }

    static NodeID zero();

    // http://bittorrent.org/beps/bep_0042.html
    static NodeID generate(asio::ip::address address);

    bool operator==(const NodeID& other) const { return buffer == other.buffer; }
    bool operator<(const NodeID& other) const { return buffer < other.buffer; }
    NodeID operator^(const NodeID& other) const;

    // Return true if `left` is closer to `this` than `right` is in the XOR
    // metrics.
    bool closer_to(const NodeID& left, const NodeID& right) const;
    NodeID distance_to(const NodeID&) const;

    private:
    static NodeID generate( asio::ip::address address
                          , boost::optional<uint8_t> test_rnd);
};

std::ostream& operator<<(std::ostream&, const NodeID&);

}} // namespaces
