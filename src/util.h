#pragma once

#include <fstream>
#include <string>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/beast/core/string_type.hpp>

#include "namespaces.h"
#include "util/signal.h"
#include "util/condition_variable.h"
#include "util/handler_tracker.h"
#include "util/url.h"
#include "or_throw.h"

namespace ouinet { namespace util {

inline
std::string canonical_url(Url urlm) {
    if (!urlm.query.empty()) urlm.query = {};
    if (!urlm.fragment.empty()) urlm.fragment = {};
    return urlm.reassemble();  // TODO: make canonical
}

// Get the source IPv4 address used when communicating with external hosts.
boost::optional<asio::ip::address> get_local_ipv4_address();

// Get the source IPv6 address used when communicating with external hosts.
boost::optional<asio::ip::address> get_local_ipv6_address();

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
    ")$";

// Matches a host string which looks like a loopback address.
// This assumes canonical IPv6 addresses (like those coming out of resolving).
// IPv6 addresses should not be bracketed.
static const boost::regex localhost_rx( _localhost_re
                                      , boost::regex::normal | boost::regex::icase);
static const boost::regex private_addr_rx( _private_addr_re
                                         , boost::regex::normal | boost::regex::icase);
#undef _IP4_LOOP_RE
#undef _IP4_PRIV1_RE
#undef _IP4_PRIV2_RE
#undef _IP4_PRIV3_RE

// Format host/port pair taking IPv6 into account.
inline
std::string format_ep(const std::string& host, const std::string& port) {
    return ( (host.find(':') == std::string::npos
              ? host // IPv4/name
              : "[" + host + "]")  // IPv6
             + ":" + port);
}

inline
std::string format_ep(const asio::ip::tcp::endpoint& ep) {
    return format_ep(ep.address().to_string(), std::to_string(ep.port()));
}

// Split into host/port pair taking IPv6 into account.
// If the host name contains no port, the second item will be empty,
// IPv6 addresses are returned without brackets.
std::pair<boost::string_view, boost::string_view>
split_ep(const boost::string_view);

///////////////////////////////////////////////////////////////////////////////
namespace detail {
std::string base32up_encode(const char*, size_t);
std::string base64_encode(const char*, size_t);
}

std::string zlib_compress(const boost::string_view&);
std::string zlib_decompress(const boost::string_view&, sys::error_code&);

template<class In>
std::string base32up_encode(const In& in) {  // unpadded!
    return detail::base32up_encode(reinterpret_cast<const char*>(in.data()), in.size());
}

std::string base32_decode(const boost::string_view);

template<class In>
std::string base64_encode(const In& in) {
    return detail::base64_encode(reinterpret_cast<const char*>(in.data()), in.size());
}

std::string base64_decode(const boost::string_view);

bool base64_decode(const boost::string_view in, uint8_t* out, size_t out_size);

template<class Array>
boost::optional<Array>
base64_decode(const boost::string_view in) {
    Array ret;
    if (!base64_decode(in, ret.data(), ret.size())) {
        return boost::none;
    }
    return ret;
}

// Returns an empty string on error (or empty input).
std::string percent_decode(const boost::string_view);

///////////////////////////////////////////////////////////////////////////////
// Conversions between various `string_view` implementations.

inline
std::string_view to_std(boost::string_view str) {
    return std::string_view(str.data(), str.size());
}

inline
boost::string_view to_boost(boost::beast::string_view str) {
    return boost::string_view(str.data(), str.size());
}

inline
boost::beast::string_view to_beast(boost::string_view str) {
    return boost::beast::string_view(str.data(), str.size());
}

inline
boost::beast::string_view to_beast(std::string_view str) {
    return boost::beast::string_view(str.data(), str.size());
}

///////////////////////////////////////////////////////////////////////////////
// Write a small file at the given `path` with a `line` of content.
// If existing, truncate it.
inline
void create_state_file(const boost::filesystem::path& path, const std::string& line) {
    std::fstream fs(path.string(), std::fstream::out | std::fstream::trunc);
    fs << line << std::endl;
    fs.close();
}

///////////////////////////////////////////////////////////////////////////////

}} // ouinet::util namespace
