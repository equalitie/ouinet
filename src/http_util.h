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
    for (auto fit = message.begin(); fit != message.end(); fit++) {
        if (!( field_is_one_of(*fit, keep_fields...)  // TODO: do case insensitive cmp
               || fit->name_string().starts_with(http_::header_prefix))) {
            fit = message.erase(fit);
        }
    }

    return message;
}

template<class Message>
static Message remove_ouinet_fields(Message message)
{
    for (auto fit = message.begin(); fit != message.end(); fit++) {
        if (fit->name_string().starts_with(http_::header_prefix)) {
            fit = message.erase(fit);
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

}} // ouinet::util namespace
