#pragma once

#include <fstream>
#include <string>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/optional.hpp>
#include <boost/beast/core/string_type.hpp>

#include "namespaces.h"
#include "util/url.h"
#include "or_throw.h"
#include "api.h"

namespace ouinet { namespace util {

// Get the source IPv4 address used when communicating with external hosts.
OUINET_COMMON_API
boost::optional<asio::ip::address> get_local_ipv4_address();

// Get the source IPv6 address used when communicating with external hosts.
OUINET_COMMON_API
boost::optional<asio::ip::address> get_local_ipv6_address();

OUINET_COMMON_API bool is_localhost_addr(const boost::string_view);
OUINET_COMMON_API bool is_private_addr(const boost::string_view);

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
OUINET_COMMON_API
std::pair<boost::string_view, boost::string_view>
split_ep(const boost::string_view);

///////////////////////////////////////////////////////////////////////////////
namespace detail {
OUINET_COMMON_API std::string base32up_encode(const char*, size_t);
OUINET_COMMON_API std::string base64_encode(const char*, size_t);
}

std::string zlib_compress(const boost::string_view&);
std::string zlib_decompress(const boost::string_view&, sys::error_code&);

template<class In>
std::string base32up_encode(const In& in) {  // unpadded!
    return detail::base32up_encode(reinterpret_cast<const char*>(in.data()), in.size());
}

OUINET_COMMON_API
std::string base32_decode(const boost::string_view);

template<class In>
std::string base64_encode(const In& in) {
    return detail::base64_encode(reinterpret_cast<const char*>(in.data()), in.size());
}

OUINET_COMMON_API
std::string base64_decode(const boost::string_view);

OUINET_COMMON_API
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
OUINET_COMMON_API
std::string percent_decode(const boost::string_view);

// Percent-encode a string (for URL query parameters).
std::string percent_encode(const boost::string_view);

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
