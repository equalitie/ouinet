#pragma once

#include <fstream>
#include <string>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/beast/core/string_type.hpp>

#include "namespaces.h"
#include "util/signal.h"
#include "util/condition_variable.h"
#include "util/handler_tracker.h"
#include "or_throw.h"

namespace ouinet { namespace util {

struct url_match {
    // Uniform Resource Identifier (URI): Generic Syntax
    // https://www.ietf.org/rfc/rfc3986.txt

    //      foo://example.com:8042/over/there?name=ferret#nose
    //      \_/   \______________/\_________/ \_________/ \__/
    //       |           |            |            |        |
    //    scheme     authority       path        query   fragment
    //
    // authority = [ userinfo "@" ] host [ ":" port ]

    std::string scheme;
    std::string host;
    std::string port;      // maybe empty
    std::string path;      // maybe empty
    std::string query;     // maybe empty
    std::string fragment;  // maybe empty

    // Rebuild the URL, dropping port, query and fragment if empty.
    std::string reassemble() const {
        auto url = boost::format("%s://%s%s%s%s%s")
            % scheme % host
            % (port.empty() ? "" : ':' + port)
            % path
            % (query.empty() ? "" : '?' + query)
            % (fragment.empty() ? "" : '#' + fragment);
        return url.str();
    }

    std::string host_and_port() const {
        if (port.empty()) {
            return host;
        } else {
            return host + ':' + port;
        }
    }
};

// Parse the HTTP URL to tell the different components.
// If successful, the `match` is updated.
bool match_http_url(const boost::string_view url, url_match& match);

// Return the canonical version of the given HTTP(S) URL
// whose match components are in `urlm`.
//
// Canonical URLs never have fragments (they should be handled by the agent).
inline
std::string canonical_url(url_match urlm) {
    if (!urlm.fragment.empty()) urlm.fragment = {};
    return urlm.reassemble();  // TODO: make canonical
}

// Return the canonical version of the given HTTP(S) `url`,
// or the empty string if it is invalid.
inline
std::string canonical_url(const boost::string_view url) {
    url_match urlm;
    if (!match_http_url(url, urlm)) return {};  // error
    return canonical_url(std::move(urlm));
}

// Get the source IPv4 address used when communicating with external hosts.
boost::optional<asio::ip::address> get_local_ipv4_address();

// Get the source IPv6 address used when communicating with external hosts.
boost::optional<asio::ip::address> get_local_ipv6_address();

inline
auto tcp_async_resolve( const std::string& host
                      , const std::string& port
                      , AsioExecutor exec
                      , Cancel& cancel
                      , asio::yield_context yield)
{
    using tcp = asio::ip::tcp;
    using Results = tcp::resolver::results_type;

    if (cancel) {
        return or_throw<Results>(yield, asio::error::operation_aborted);
    }

    // Note: we're spawning a new coroutine here and deal with all this
    // ConditionVariable machinery because - contrary to what Asio's
    // documentation says - resolver::async_resolve isn't immediately
    // cancelable. I.e.  when resolver::async_resolve is running and
    // resolver::cancel is called, it is not guaranteed that the async_resolve
    // call gets placed on the io_service queue immediately. Instead, it was
    // observed that this can in some rare cases take more than 20 seconds.
    //
    // Also note that this is not Asio's fault. Asio uses internally the
    // getaddrinfo() function which doesn't support cancellation.
    //
    // https://stackoverflow.com/questions/41352985/abort-a-call-to-getaddrinfo
    sys::error_code ec;
    Results results;
    ConditionVariable cv(exec);
    tcp::resolver* rp = nullptr;

    auto cancel_lookup_slot = cancel.connect([&] {
        ec = asio::error::operation_aborted;
        cv.notify();
        if (rp) rp->cancel();
    });

    bool* finished_p = nullptr;

    TRACK_SPAWN(exec, ([&] (asio::yield_context yield) {
        bool finished = false;
        finished_p = &finished;

        tcp::resolver resolver{exec};
        rp = &resolver;
        sys::error_code ec_;
        auto r = resolver.async_resolve(host, port, yield[ec_]);
        // Turn this confusing resolver error into something more understandable.
        static const sys::error_code busy_ec{ sys::errc::device_or_resource_busy
                                            , sys::system_category()};
        if (ec_ == busy_ec) ec_ = asio::error::host_not_found;

        if (finished) return;

        rp = nullptr;
        results = std::move(r);
        ec = ec_;
        finished_p = nullptr;
        cv.notify();
    }));

    cv.wait(yield);

    if (finished_p) *finished_p = true;

    ec = compute_error_code(ec, cancel);
    return or_throw(yield, ec, std::move(results));
}

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
static const std::string _private_re =
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
static const boost::regex private_rx( _private_re
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
