#pragma once

#include "namespaces.h"
#include <string>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include "constants.h"
#include "util.h"

namespace ouinet {

namespace util {

// Get the host and port a request refers to,
// either from the ``Host:`` header or from the target URI.
// IPv6 addresses are returned without brackets.
std::pair<std::string, std::string>
get_host_port(const http::request<http::string_body>&);

///////////////////////////////////////////////////////////////////////////////
template<class Num>
Num parse_num(beast::string_view s, Num default_value) {
    try {
        return boost::lexical_cast<Num>(s);
    }
    catch (...) {
        return default_value;
    }
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
static Message remove_ouinet_fields(Message message)
{
    for (auto fit = message.begin(); fit != message.end();) {
        if (boost::istarts_with(fit->name_string(), http_::header_prefix)) {
            fit = message.erase(fit);
        } else {
            fit++;
        }
    }

    return message;
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

// Make the given request canonical.
//
// This only leaves a minimum set of non-privacy sensitive headers,
// and some of them may be altered for cacheability or privacy reasons.
//
// Internal Ouinet headers and headers in `keep_fields` are also kept.
template<class Request, class... Fields>
static Request to_canonical_request(Request rq, const Fields&... keep_fields) {
    auto url = canonical_url(rq.target());
    rq.target(url);
    rq.version(11);  // HTTP/1.1

    // Some canonical header values that need ADD, KEEP or PROCESS.
    url_match urlm;
    match_http_url(url, urlm);  // assume check by `canonical_url` above
    rq.set(http::field::host, urlm.host);
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
template<class Request>
static Request to_injector_request(Request rq) {
    // The Ouinet version header hints the endpoint
    // to behave like an injector instead of a proxy.
    rq.set(http_::request_version_hdr, http_::request_version_hdr_current);
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
template<class Request>
static Request to_cache_request(Request rq) {
    rq = remove_ouinet_fields(move(rq));
    return to_canonical_request(move(rq));
}

// Make the given response ready to be sent to the cache.
// This only leaves a minimum set of non-privacy sensitive headers.
template<class Response>
static Response to_cache_response(Response rs) {
    rs = remove_ouinet_fields(move(rs));
    // TODO: This list was created by going through some 100 responses from
    // bbc.com. Careful selection from all possible (standard) fields is
    // needed.
    return filter_fields( move(rs)
                        , http::field::server
                        , http::field::retry_after
                        , http::field::content_length
                        , http::field::content_type
                        , http::field::content_encoding
                        , http::field::content_language
                        , http::field::transfer_encoding
                        , http::field::accept_ranges
                        , http::field::etag
                        , http::field::age
                        , http::field::date
                        , http::field::expires
                        , http::field::via
                        , http::field::vary
                        , http::field::location
                        , http::field::cache_control
                        , http::field::warning
                        , http::field::last_modified
                        // # CORS response headers (following <https://fetch.spec.whatwg.org/#http-responses>)
                        , http::field::access_control_allow_origin  // origins the response may be shared with
                        // A request which caused a response with ``Access-Control-Allow-Credentials: true``
                        // probably carried authentication tokens and it should not have been cached anyway,
                        // however a server may erroneously include it for requests not using credentials,
                        // and we do not want to block them.
                        // See <https://stackoverflow.com/a/24689738> for an explanation of the header.
                        , http::field::access_control_allow_credentials  // resp to req w/credentials may be shared
                        // These response headers should only appear in
                        // responses to pre-flight (OPTIONS) requests, which should not be cached.
                        // However, some servers include them as part of responses to GET requests,
                        // so include them since they are not problematic either.
                        , http::field::access_control_allow_methods  // methods allowed in CORS request
                        , http::field::access_control_allow_headers  // headers allowed in CORS request
                        , http::field::access_control_max_age  // expiration of pre-flight response info
                        , http::field::access_control_expose_headers  // headers of response to be exposed
                        );
}

}} // ouinet::util namespace
