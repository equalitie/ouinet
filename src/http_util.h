#pragma once

#include "namespaces.h"
#include <string>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "constants.h"
#include "default_timeout.h"
#include "or_throw.h"
#include "util.h"
#include "util/signal.h"
#include "util/watch_dog.h"

namespace ouinet {

namespace util {

// Get the host and port a request refers to,
// either from the ``Host:`` header or from the target URI.
// IPv6 addresses are returned without brackets.
std::pair<std::string, std::string>
get_host_port(const http::request<http::string_body>&);

///////////////////////////////////////////////////////////////////////////////
// Helps parsing and printing contents of `Content-Range` headers.
struct HttpResponseByteRange {
    size_t first;
    size_t last;
    // Total size of the document (if known)
    boost::optional<size_t> length;

    static
    boost::optional<HttpResponseByteRange>
    parse(boost::string_view);

    bool
    matches_length(size_t) const;

    bool
    matches_length(boost::string_view) const;
};

std::ostream&
operator<<(std::ostream&, const HttpResponseByteRange&);

struct HttpRequestByteRange {
    size_t first;
    size_t last;

    // Returns boost::none on parse error
    static
    boost::optional<std::vector<HttpRequestByteRange>>
    parse(boost::string_view);
};

///////////////////////////////////////////////////////////////////////////////
// Returns ptime() if parsing fails.
boost::posix_time::ptime parse_date(beast::string_view);
std::string format_date(boost::posix_time::ptime);

// Return empty is missing or malformed.
boost::string_view http_injection_field( const http::response_header<>&
                                       , const std::string&);

inline
boost::string_view http_injection_id(const http::response_header<>& rsh)
{
    return http_injection_field(rsh, "id");
}

inline
boost::string_view http_injection_ts(const http::response_header<>& rsh)
{
    return http_injection_field(rsh, "ts");
}

// Send the HTTP request `rq` over `in`,
// trigger an error on timeout or cancellation,
// closing `in`.
template<class StreamIn, class Request>
inline
void
http_request( StreamIn& in
            , const Request& rq
            , Cancel& cancel
            , asio::yield_context yield)
{
    auto cancelled = cancel.connect([&] { in.close(); });
    bool timed_out = false;
    sys::error_code ec;

    WatchDog wdog( in.get_executor(), default_timeout::http_request()
                 , [&] { timed_out = true; in.close(); });
    http::async_write(in, rq, yield[ec]);

    // Ignore `end_of_stream` error, there may still be data in
    // the receive buffer we can read.
    if (ec == http::error::end_of_stream)
        ec = sys::error_code();
    if (timed_out)
        ec = asio::error::timed_out;
    if (cancelled)
        ec = asio::error::operation_aborted;
    if (ec)
        return or_throw(yield, ec);
}

///////////////////////////////////////////////////////////////////////////////
namespace detail {
    boost::optional<http::response<http::empty_body>>
    http_proto_version_error( unsigned rv
                            , beast::string_view ov
                            , beast::string_view ss);
}

// Return an error response message if
// the request contains a protocol version number not matching the current one.
template<class Request>
inline
boost::optional<http::response<http::empty_body>>
http_proto_version_error( const Request& rq
                        , beast::string_view oui_version
                        , beast::string_view server_string)
{
    return detail::http_proto_version_error( rq.version()
                                           , oui_version
                                           , server_string);
}

template<class Request>
inline
boost::optional<http::response<http::empty_body>>
http_proto_version_error( const Request& rq
                        , beast::string_view server_string)
{
    return http_proto_version_error( rq
                                   , rq[http_::protocol_version_hdr]
                                   , server_string);
}

namespace detail {
    bool http_proto_version_check_trusted(boost::string_view, unsigned&);
}

// Does the `message` contain a usable Ouinet protocol version?
//
// Also set `newest_proto_seen` if the `message` contains a greater value,
// so only call this with a `message` coming from a trusted source.
template<class Message>
inline
bool http_proto_version_check_trusted( const Message& message
                                     , unsigned& newest_proto_seen)
{
    return detail::http_proto_version_check_trusted
        ( message[http_::protocol_version_hdr]
        , newest_proto_seen);
}

// Create an HTTP client error response for the given request `rq`
// with the given `status` and `message` body (text/plain).
// If `proto_error` is not empty,
// make this a Ouinet protocol message with that error.
template<class Request>
inline
http::response<http::string_body>
http_client_error( const Request& rq
                 , http::status status
                 , const std::string& proto_error
                 , const std::string& message = "")
{
    http::response<http::string_body> rs{status, rq.version()};

    if (!proto_error.empty()) {
        assert(boost::regex_match(proto_error, http_::response_error_rx));
        rs.set(http_::protocol_version_hdr, http_::protocol_version_hdr_current);
        rs.set(http_::response_error_hdr, proto_error);
    }
    rs.set(http::field::server, OUINET_CLIENT_SERVER_STRING);
    rs.set(http::field::content_type, "text/plain");
    rs.keep_alive(rq.keep_alive());
    rs.body() = message;
    rs.prepare_payload();

    return rs;
}

///////////////////////////////////////////////////////////////////////////////
// Utility function to check whether an HTTP field belongs to a set. Where
// the set is defined by second, third, fourth,... arguments.
// E.g.:
//
// bool has_cookie_or_date_field(const http::response_header<>& header) {
//     for (auto& f : header) {
//         if (field_is_one_of(f, http::field::cookie, http::field::date) {
//             return true;
//         }
//     }
// }
//
// Note, it's done with the help of template specialization because
// not all (mostly non standard) fields are mentioned in the http::field
// enum. Thus, with this approach we can do:
// `field_is_one_of(f, http::field::cookie, "Upgrade-Insecure-Requests")`

namespace detail_field_is_one_of {
    // Specialized compare functions for http::field and char[].

    template<class> struct Compare;

    template<> struct Compare<http::field> {
        static bool is_same(const http::fields::value_type& f1, http::field f2) {
            return f1.name() == f2;
        }
    };

    template<size_t N> struct Compare<char[N]> {
        static bool is_same(const http::fields::value_type& f1, const char* f2) {
            return boost::iequals(f1.name_string(), f2);
        }
    };
} // detail_field_is_one_of namespace

inline
bool field_is_one_of(const http::fields::value_type&) {
    return false;
}

template<class First,  class... Rest>
inline
bool field_is_one_of(const http::fields::value_type& e,
                     const First& first,
                     const Rest&... rest)
{
    if (detail_field_is_one_of::Compare<First>::is_same(e, first)) return true;
    return field_is_one_of(e, rest...);
}

///////////////////////////////////////////////////////////////////////////////
// Remove all fields that are not listed in `keep_fields`,
// nor are Ouinet internal headers.
template<class Message, class... Fields>
static Message filter_fields(Message message, const Fields&... keep_fields)
{
    for (auto fit = message.begin(); fit != message.end();) {
        if (!( field_is_one_of(*fit, keep_fields...)
               || boost::istarts_with(fit->name_string(), http_::header_prefix))) {
            fit = message.erase(fit);
        } else {
            fit++;
        }
    }

    return message;
}

template<class Message>
static void remove_ouinet_fields_ref(Message& message)
{
    for (auto fit = message.begin(); fit != message.end();) {
        if (boost::istarts_with(fit->name_string(), http_::header_prefix)) {
            fit = message.erase(fit);
        } else {
            fit++;
        }
    }
}

template<class Message>
static Message remove_ouinet_fields(Message message)
{
    remove_ouinet_fields_ref(message);
    return message;
}

template<class Response>
static Response to_non_chunked_response(Response rs) {
    rs.chunked(false);
    rs.set(http::field::content_length, rs.body().size());
    rs.erase(http::field::trailer);  // pointless without chunking
    return rs;
}

// Transform request from absolute-form to origin-form
// https://tools.ietf.org/html/rfc7230#section-5.3
template<class Request>
Request req_form_from_absolute_to_origin(const Request& absolute_req)
{
    // Parse the URL to tell HTTP/HTTPS, host, port.
    url_match url;

    auto absolute_target = absolute_req.target();

    if (!match_http_url(absolute_target, url)) {
        assert(0 && "Failed to parse url");
        return absolute_req;
    }

    Request origin_req(absolute_req);

    origin_req.target(absolute_target.substr(
                absolute_target.find( url.path
                                    // Length of "http://" or "https://",
                                    // do not fail on "http(s)://FOO/FOO".
                                    , url.scheme.length() + 3)));

    return origin_req;
}

namespace detail {
    std::string http_host_header(const std::string&, const std::string&);
}

// Add a `Host:` header to `req` if missing or empty.
//
// If the header continues to be empty after the call,
// the request is invalid (e.g. missing host and bad target).
template<class Request>
void req_ensure_host(Request& req) {
    if (!req[http::field::host].empty()) return;

    std::string host, port;
    std::tie(host, port) = util::get_host_port(req);
    auto hosth = detail::http_host_header(host, port);
    if (hosth.empty()) return;  // error
    req.set(http::field::host, hosth);
}

// Make the given request canonical.
//
// This only leaves a minimum set of non-privacy sensitive headers,
// and some of them may be altered for cacheability or privacy reasons.
//
// Internal Ouinet headers and headers in `keep_fields` are also kept.
//
// If the request is invalid, none is returned.
template<class Request, class... Fields>
static boost::optional<Request>
to_canonical_request(Request rq, const Fields&... keep_fields) {
    auto url = rq.target();
    url_match urlm;
    if (!match_http_url(url, urlm)) return boost::none;
    rq.target(canonical_url(urlm));
    rq.version(11);  // HTTP/1.1

    // Some canonical header values that need ADD, KEEP or PROCESS.
    rq.set( http::field::host
          , (urlm.port.empty() ? urlm.host : urlm.host + ":" + urlm.port));
    rq.set(http::field::accept, "*/*");
    rq.set(http::field::accept_encoding, "");
    rq.set("DNT", "1");
    rq.set("Upgrade-Insecure-Requests", "1");
    rq.set( http::field::user_agent
          , "Mozilla/5.0 (Windows NT 6.1; rv:60.0) Gecko/20100101 Firefox/60.0");

    // Basically only keep headers which are absolutely necessary,
    // do not break privacy and can not break browsing for others.
    // For the moment we do not yet care about
    // requests coming from Ouinet injector being fingerprinted as such.
    return filter_fields( move(rq)
                        // Still DROP some fields that may break browsing for others
                        // and which have no sensible default (for all).
                        , http::field::host
                        , http::field::accept
                        //, http::field::accept_datetime  // DROP
                        , http::field::accept_encoding
                        //, http::field::accept_language  // DROP
                        , "DNT"
                        , http::field::from
                        , http::field::origin
                        , "Upgrade-Insecure-Requests"
                        , http::field::user_agent
                        , keep_fields...
                        );
}

// Make the given request ready to be sent to the injector.
//
// This means a canonical request with internal Ouinet headers,
// plus proxy authorization headers and caching headers.
//
// If the request is invalid, none is returned.
template<class Request>
static boost::optional<Request>
to_injector_request(Request rq) {
    // The Ouinet version header hints the endpoint
    // to behave like an injector instead of a proxy.
    rq.set(http_::protocol_version_hdr, http_::protocol_version_hdr_current);
    // Some cache back-ends may use trailers for hashes, signatures, etc.
    rq.set(http::field::te, "trailers");
    return to_canonical_request( move(rq)
                               // PROXY AUTHENTICATION HEADERS (PASS)
                               , http::field::proxy_authorization
                               // CACHING AND RANGE HEADERS (PASS)
                               , http::field::cache_control
                               , http::field::if_match
                               , http::field::if_modified_since
                               , http::field::if_none_match
                               , http::field::if_range
                               , http::field::if_unmodified_since
                               , http::field::pragma
                               , http::field::range
                               );
}

// Make the given request ready to be sent to the origin by
// using origin request target form (<https://tools.ietf.org/html/rfc7230#section-5.3.1>),
// removing Ouinet-specific internal HTTP headers and
// proxy authorization headers.
//
// The rest of headers are left intact.
template<class Request>
static Request to_origin_request(Request rq) {
    rq = req_form_from_absolute_to_origin(move(rq));
    rq.erase(http::field::proxy_authorization);
    return remove_ouinet_fields(move(rq));
}

// Make the given request ready to be sent to the cache.
//
// This means a canonical request with no additional headers.
//
// If the request is invalid, none is returned.
template<class Request>
static boost::optional<Request>
to_cache_request(Request rq) {
    rq = remove_ouinet_fields(move(rq));
    return to_canonical_request(move(rq));
}

// Make the given response ready to be sent to the cache.
// This only leaves a minimum set of non-privacy sensitive headers.
// An error code may be set if the response can not be safely converted to
// a cache response.
http::response_header<> to_cache_response(http::response_header<>, sys::error_code&);

template<class Body>
static http::response<Body> to_cache_response(http::response<Body> rs, sys::error_code& ec) {
    // Disable chunked transfer encoding and use actual body size as content length.
    // This allows sharing the plain body representation with other platforms.
    // It also compensates for the lack of body data size field in v0 descriptors.
    rs = to_non_chunked_response(move(rs));

    auto rsh = to_cache_response(move(rs.base()), ec);
    return http::response<Body>(move(rsh), move(rs.body()));
}

http::fields to_cache_trailer(http::fields rst);

}} // ouinet::util namespace
