#pragma once

#include <boost/asio/ip/address.hpp>
#include <string>
#include <array>
#include "../namespaces.h"

namespace ouinet { namespace bittorrent {

struct NodeID {
    std::array<unsigned char, 20> buffer;

    // XXX: `bit(0)` is the most signifficant, perhaps the function should be
    // called `rbit` ('r' for reverse)?
    bool bit(int n) const;
    void set_bit(int n, bool value);

    std::string to_hex() const;
    std::string to_bytestring() const;
    static NodeID from_bytestring(const std::string& bytestring);
    static const NodeID& zero();
    static NodeID generate(asio::ip::address address);

    /*
     * Generate a random NodeID with first $stencil_mask number of
     * most signifficant bits equal to those in $stencil.
     */
    static NodeID random(const NodeID& stencil, size_t stencil_mask);

    bool operator==(const NodeID& other) const { return buffer == other.buffer; }
};

}} // namespaces
