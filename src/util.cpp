#include "util.h"

#include <algorithm>
#include <iterator>

#include <boost/asio/ip/udp.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/regex.hpp>

#include <boost/url/encode.hpp>
#include <boost/url/decode_view.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>

#include "util/iterators/base32_from_binary.hpp"
#include "util/iterators/binary_from_base32.hpp"

namespace ouinet::util {

using namespace std;

#define _IP4_LOOP_RE "127(?:\\.[0-9]{1,3}){3}"
static const std::string _localhost_re =
    "^(?:"
    "(?:localhost|ip6-localhost|ip6-loopback)(?:\\.localdomain)?"
    "|" _IP4_LOOP_RE         // IPv4, e.g. 127.1.2.3
    "|::1"                   // IPv6 loopback
    "|::ffff:" _IP4_LOOP_RE  // IPv4-mapped IPv6
    "|::" _IP4_LOOP_RE       // IPv4-compatible IPv6
    ")$";

#define _IP4_PRIV1_RE "10(?:\\.[0-9]{1,3}){3}"
#define _IP4_PRIV2_RE "172\\.(1[6-9]|2[0-9]|3[0-1])(?:\\.[0-9]{1,3}){2}"
#define _IP4_PRIV3_RE "192\\.168(?:\\.[0-9]{1,3}){2}"
#define _IP4_PRIV4_RE "169\\.254(?:\\.[0-9]{1,3}){2}"
#define _IP6_PRIV1_RE "fe80::(?:%[0-9a-zA-Z]+)?"
#define _IP6_PRIV2_RE "fe80:(?:(?:[0-9a-f]{1,4}:)*:(?:[0-9a-f]{1,4}:)*[0-9a-f]{1,4}|(?:[0-9a-f]{1,4}:){1,7}[0-9a-f]{1,4})(?:%[0-9a-zA-Z]+)?"
#define _IP6_PRIV3_RE "f[cd][0-9a-f]{2}::(?:%[0-9a-zA-Z]+)?"
#define _IP6_PRIV4_RE "f[cd][0-9a-f]{2}:(?:(?:[0-9a-f]{1,4}:)*:(?:[0-9a-f]{1,4}:)*[0-9a-f]{1,4}|(?:[0-9a-f]{1,4}:){1,7}[0-9a-f]{1,4})(?:%[0-9a-zA-Z]+)?"
static const std::string _private_addr_re =
    "^(?:"
    "|" _IP4_PRIV1_RE         // IPv4, e.g. 10.8.4.2
    "|::ffff:" _IP4_PRIV1_RE  // IPv4-mapped IPv6
    "|::" _IP4_PRIV1_RE       // IPv4-compatible IPv6
    "|" _IP4_PRIV2_RE         // IPv4, e.g. 172.17.0.1
    "|::ffff:" _IP4_PRIV2_RE  // IPv4-mapped IPv6
    "|::" _IP4_PRIV2_RE       // IPv4-compatible IPv6
    "|" _IP4_PRIV3_RE         // IPv4, e.g. 192.168.2.3
    "|::ffff:" _IP4_PRIV3_RE  // IPv4-mapped IPv6
    "|::" _IP4_PRIV3_RE       // IPv4-compatible IPv6
    "|" _IP4_PRIV4_RE         // IPv4, e.g. 169.254.2.3
    "|::ffff:" _IP4_PRIV4_RE  // IPv4-mapped IPv6
    "|::" _IP4_PRIV4_RE       // IPv4-compatible IPv6
    "|" _IP6_PRIV1_RE         // IPv6 link-local compact
    "|" _IP6_PRIV2_RE         // IPv6 link-local
    "|" _IP6_PRIV3_RE         // IPv6 unique-local compact
    "|" _IP6_PRIV4_RE         // IPv6 unique-local
    ")$";

OUINET_COMMON_API bool is_localhost_addr(const boost::string_view s) {
    // Matches a host string which looks like a loopback address.
    // This assumes canonical IPv6 addresses (like those coming out of resolving).
    // IPv6 addresses should not be bracketed.
    static const boost::regex localhost_rx( _localhost_re
                                          , boost::regex::normal | boost::regex::icase);
    return boost::regex_match(s.begin(), s.end(), localhost_rx);
}

OUINET_COMMON_API bool is_private_addr(const boost::string_view s) {
    static const boost::regex private_addr_rx( _private_addr_re
                                             , boost::regex::normal | boost::regex::icase);
    return boost::regex_match(s.begin(), s.end(), private_addr_rx);
}

#undef _IP4_LOOP_RE
#undef _IP4_PRIV1_RE
#undef _IP4_PRIV2_RE
#undef _IP4_PRIV3_RE
#undef _IP4_PRIV4_RE
#undef _IP6_PRIV1_RE
#undef _IP6_PRIV2_RE

boost::optional<boost::asio::ip::address>
get_local_ip_address(const boost::asio::ip::udp::endpoint& ep) {
    boost::asio::io_context ctx;
    boost::asio::ip::udp::socket s(ctx, ep.protocol());
    boost::system::error_code ec;
    s.connect(ep, ec);
    if (ec) return boost::none;
    return s.local_endpoint().address();
}

boost::optional<boost::asio::ip::address>
get_local_ipv4_address() {
    using namespace boost::asio::ip;
    static const udp::endpoint ep(make_address_v4("192.0.2.1"), 1234);
    return get_local_ip_address(ep);
}

boost::optional<boost::asio::ip::address>
get_local_ipv6_address() {
    using namespace boost::asio::ip;
    static const udp::endpoint ep(make_address_v6("2001:db8::1"), 1234);
    return get_local_ip_address(ep);
}

std::pair<boost::string_view, boost::string_view>
split_ep(const boost::string_view ep) {
    if (ep.empty()) return {};

    boost::string_view host, port;

    auto cpos = ep.rfind(':');

    if (cpos == string::npos || ep[ep.length() - 1] == ']') {
        host = ep;
    } else {
        host = ep.substr(0, cpos);
        port = ep.substr(cpos + 1);
    }

    // Remove brackets from IPv6 hosts.
    if (host[0] == '[')
        host = host.substr(1, host.length() - 2);

    return make_pair(host, port);
}

// Chaining the following two with something like
// <https://github.com/sneumann/mzR/blob/master/src/boost_aux/boost/iostreams/filter/base64.hpp>
// would be way cooler.`:)`

// Based on <https://stackoverflow.com/a/27559848> by user "Zangetsu".
template <class Filter>
string zlib_filter(const boost::string_view& in) {
    stringstream in_ss;
    in_ss << in;

    boost::iostreams::filtering_streambuf<boost::iostreams::input> zip;
    zip.push(Filter());
    zip.push(in_ss);

    ostringstream out_ss;
    boost::iostreams::copy(zip, out_ss);
    return out_ss.str();
}

string zlib_compress(const boost::string_view& in) {
    return zlib_filter<boost::iostreams::zlib_compressor>(in);
}

// TODO: Catch and report decompression errors.
string zlib_decompress(const boost::string_view& in, sys::error_code& ec) {
    return zlib_filter<boost::iostreams::zlib_decompressor>(in);
}

// Based on <https://stackoverflow.com/a/28471421> by user "ltc"
// and <https://stackoverflow.com/a/10973348> by user "PiQuer".
string detail::base32up_encode(const char* data, size_t size) {
    using namespace boost::archive::iterators;
    using It = base32_from_binary<transform_width<const char*, 5, 8>>;
    It begin = data;
    It end   = data + size;
    string out(begin, end);  // encode to base32
    return out;  // do not add padding
}

string base32_decode(const boost::string_view in) {
    using namespace boost::archive::iterators;
    using It = transform_width<binary_from_base32<const char*>, 8, 5>;
    It begin = in.data();
    It end   = in.data() + in.size();
    string out(begin, end);  // decode from base32
    size_t npad = count(in.begin(), in.end(), '=');
    if (npad > 6) npad = 6;
    static const size_t padding_bytes[7] = {0, 1, 1, 2, 3, 3, 4};
    npad = padding_bytes[npad];
    return out.erase((npad > out.size()) ? 0 : out.size() - npad);  // remove padding
}

// Based on <https://stackoverflow.com/a/28471421> by user "ltc"
// and <https://stackoverflow.com/a/10973348> by user "PiQuer".
string detail::base64_encode(const char* data, size_t size) {
    using namespace boost::archive::iterators;
    using It = base64_from_binary<transform_width<const char*, 6, 8>>;
    It begin = data;
    It end   = data + size;
    string out(begin, end);  // encode to base64
    return out.append((3 - size % 3) % 3, '=');  // add padding
}

string base64_decode(const boost::string_view in) {
    using namespace boost::archive::iterators;
    using It = transform_width<binary_from_base64<const char*>, 8, 6>;
    It begin = in.data();
    It end   = in.data() + in.size();
    string out(begin, end);  // decode from base64
    size_t npad = count(in.begin(), in.end(), '=');
    return out.erase((npad > out.size()) ? 0 : out.size() - npad);  // remove padding
}

bool base64_decode(boost::string_view in, uint8_t* out, size_t out_size) {
    using namespace boost::archive::iterators;
    using It = transform_width<binary_from_base64<const char*>, 8, 6>;

    while (in.ends_with('=')) in.remove_suffix(1);

    // Note: iterating over this range results in an infinite loop unless
    // the value of the iterator is accessed (e.g. with *i).
    It begin = in.data();
    It end   = in.data() + in.size();

    size_t size = 0;

    for (auto i = begin; i != end && size < out_size; ++out, ++i, ++size) {
        *out = *i;
    }

    return size == out_size;
}

string percent_decode(const boost::string_view in) {
    if (in.empty()) return {};
    boost::urls::decode_view dv(in);
    return std::string(dv.begin(), dv.end());
}

string percent_encode(const boost::string_view in) {
    if (in.empty()) return {};
    namespace urls = boost::urls;
    auto ev = urls::encode(in, urls::unreserved_chars);
    return std::string(ev.begin(), ev.end());
}

} // namespace
