#include <boost/crc.hpp>
#include "node_id.h"

using namespace ouinet::bittorrent;

static bool get_rbit(const NodeID::Buffer& buffer, size_t n) {
    return (buffer[n / CHAR_BIT] & (1 << (CHAR_BIT - 1 - (n % CHAR_BIT)))) != 0;
}

static void set_rbit(NodeID::Buffer& buffer, size_t n, bool value) {
    char bit = 1 << (CHAR_BIT - (n % CHAR_BIT) - 1);

    if (value) buffer[n / CHAR_BIT] |=  bit;
    else       buffer[n / CHAR_BIT] &= ~bit;
}

static const NodeID::Buffer& zero_buffer() {
    // zero-initialized by static linkage
    static NodeID::Buffer buf;
    return buf;
}

bool NodeID::bit(int n) const
{
    return get_rbit(buffer, n);
}

void NodeID::set_bit(int n, bool value)
{
    set_rbit(buffer, n, value);
}

const NodeID::Range& NodeID::Range::max()
{
    static Range max_range{ zero_buffer(), 0 };
    return max_range;
}

NodeID NodeID::Range::random_id() const
{
    // XXX: Use std::uniform_int_distribution instead of std::rand

    size_t s_bytes = mask / CHAR_BIT;
    size_t s_bits  = mask % CHAR_BIT;

    NodeID ret;

    for (size_t i = 0; i < ret.buffer.size(); i++) {
        if (i < s_bytes) {
            ret.buffer[i] = stencil[i];
        }
        else if (i > s_bytes) {
            ret.buffer[i] = rand() & 0xff;
        }
        else {
            ret.buffer[i] = (stencil[i] & ((0xff << (CHAR_BIT - s_bits)) & 0xff))
                          | (rand() & ((1 << (CHAR_BIT - s_bits)) - 1));
        }
    }

    return ret;
}

NodeID::Range NodeID::Range::reduce(bool bit) const {
    Range ret{stencil, mask};
    ++ret.mask;
    set_rbit(ret.stencil, ret.mask, bit);
    return ret;
}

NodeID NodeID::zero()
{
    return NodeID{ zero_buffer() };
}

NodeID NodeID::generate(asio::ip::address address)
{
    return generate(address, boost::none);
}

NodeID NodeID::generate( asio::ip::address address
                       , boost::optional<uint8_t> test_rnd)
{
    /*
     * Choose DHT ID based on ip address.
     * See: BEP 42
     */

    NodeID node_id;

    using CRC32C = boost::crc_optimal<32, 0x1edc6f41, 0xffffffff, 0xffffffff, true, true>;

    uint32_t checksum;

    node_id.buffer[19] = (test_rnd ? *test_rnd : std::rand()) & 0xff;

    if (address.is_v4()) {
        auto ip_bytes = address.to_v4().to_bytes();

        for (int i = 0; i < 4; i++) {
            ip_bytes[i] &= (0xff >> (6 - i * 2));
        }

        ip_bytes[0] |= ((node_id.buffer[19] & 7) << 5);

        CRC32C crc;
        crc.process_bytes(ip_bytes.data(), 4);
        checksum = crc.checksum();
    } else {
        auto ip_bytes = address.to_v6().to_bytes();

        for (int i = 0; i < 8; i++) {
            ip_bytes[i] &= (0xff >> (7 - i));
        }

        ip_bytes[0] |= ((node_id.buffer[19] & 7) << 5);

        CRC32C crc;
        crc.process_bytes(ip_bytes.data(), 8);
        checksum = crc.checksum();
    }

    node_id.buffer[0] = (checksum >> 24) & 0xff;
    node_id.buffer[1] = (checksum >> 16) & 0xff;
    node_id.buffer[2] = ((checksum >> 8) & 0xf8) | (rand() & 0x7);
    for (int i = 3; i < 19; i++) {
        node_id.buffer[i] = rand() & 0xff;
    }

    return node_id;
}

bool NodeID::closer_to(const NodeID& left, const NodeID& right) const
{
    for (size_t i = 0; i < sizeof(buffer); i++) {
        uint8_t l = left .buffer[i] ^ buffer[i];
        uint8_t r = right.buffer[i] ^ buffer[i];
        if (l < r) {
            return true;
        }
        if (r < l) {
            return false;
        }
    }
    return false;
}

NodeID NodeID::operator^(const NodeID& other) const
{
    NodeID ret;
    for (size_t i = 0; i < sizeof(buffer); i++) {
        ret.buffer[i] = buffer[i] ^ other.buffer[i];
    }
    return ret;
}

NodeID NodeID::distance_to(const NodeID& other) const
{
    NodeID ret = NodeID::zero();
    for (size_t i = 0; i < sizeof(buffer); i++) {
        ret.buffer[i] = buffer[i] ^ other.buffer[i];
    }
    return ret;
}

std::ostream& ouinet::bittorrent::operator<<(std::ostream& os, const NodeID& id)
{
    return os << id.to_hex();
}
