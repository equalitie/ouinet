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
    bool was_zeroed = false;
    static NodeID::Buffer buf;

    if (!was_zeroed) {
        memset(buf.data(), 0, buf.size());
        was_zeroed = true;
    }

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

std::string NodeID::to_hex() const
{
    std::string output;
    for (unsigned int i = 0; i < buffer.size(); i++) {
        const char* digits = "0123456789abcdef";
        output += digits[(buffer[i] >> 4) & 0xf];
        output += digits[(buffer[i] >> 0) & 0xf];
    }
    return output;
}

std::string NodeID::to_bytestring() const
{
    return std::string((char*) buffer.data(), buffer.size());
}

NodeID NodeID::from_bytestring(const std::string& bytestring)
{
    NodeID output;
    std::copy(bytestring.begin(), bytestring.end(), output.buffer.begin());
    return output;
}

const NodeID& NodeID::zero()
{
    static const NodeID ret = from_bytestring(std::string(20, '\0'));
    return ret;
}

NodeID NodeID::generate(asio::ip::address address)
{
    /*
     * Choose DHT ID based on ip address.
     * See: BEP 42
     */

    NodeID node_id;

    uint32_t checksum;
    node_id.buffer[19] = rand() & 0xff;
    if (address.is_v4()) {
        std::array<unsigned char, 4> ip_bytes = address.to_v4().to_bytes();
        for (int i = 0; i < 4; i++) {
            ip_bytes[i] &= (0xff >> (6 - i * 2));
        }
        ip_bytes[0] |= ((node_id.buffer[19] & 7) << 5);

        boost::crc_optimal<32, 0x1edc6f41, 0xffffffff, 0xffffffff, true, true> crc;
        crc.process_bytes(ip_bytes.data(), 4);
        checksum = crc.checksum();
    } else {
        std::array<unsigned char, 16> ip_bytes = address.to_v6().to_bytes();
        for (int i = 0; i < 8; i++) {
            ip_bytes[i] &= (0xff >> (7 - i));
        }
        ip_bytes[0] |= ((node_id.buffer[19] & 7) << 5);

        boost::crc_optimal<32, 0x1edc6f41, 0xffffffff, 0xffffffff, true, true> crc;
        crc.process_bytes(ip_bytes.data(), 8);
        checksum = crc.checksum();
    }

    node_id.buffer[0] = (checksum >> 24) & 0xff;
    node_id.buffer[1] = (checksum >> 16) & 0xff;
    node_id.buffer[2] = ((checksum >>  8) & 0xe0) | (rand() & 0x1f);
    for (int i = 3; i < 19; i++) {
        node_id.buffer[i] = rand() & 0xff;
    }

    return node_id;
}

