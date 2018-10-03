#pragma once

#include "namespaces.h"
#include <string>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast/http/fields.hpp>

namespace ouinet {

namespace http_ {
// TODO: This should be called ``http``,
// but it is already being used as an alias for ``boost::http``.

// Common prefix for all Ouinet-specific internal HTTP headers.
static const std::string header_prefix = "X-Ouinet-";
// The presence of this (non-empty) HTTP request header
// shows the protocol version used by the client
// and hints the receiving injector to behave like an injector instead of a proxy.
static const std::string request_version_hdr = header_prefix + "Version";
static const std::string request_version_hdr_v0 = "0";
static const std::string request_version_hdr_latest = request_version_hdr_v0;
// Such a request should get the following HTTP response header
// with an opaque identifier for this insertion.
static const std::string response_injection_id_hdr = header_prefix + "Injection-ID";
// The presence of this HTTP request header with the true value below
// instructs the injector to behave synchronously
// and inline the resulting descriptor in response headers.
static const std::string request_sync_injection_hdr = header_prefix + "Sync";
static const std::string request_sync_injection_true = "true";
// If synchronous injection is enabled in an HTTP request,
// this header is added to the resulting response
// with the Base64-encoded, Zlib-compressed content of the descriptor.
static const std::string response_descriptor_hdr = header_prefix + "Descriptor";

} // ouinet::http_ namespace

namespace util {

inline
std::pair< beast::string_view
         , beast::string_view
         >
split_host_port(const beast::string_view& hp)
{
    using namespace std;

    auto pos = hp.find(':');

    if (pos == string::npos) {
        return make_pair(hp, "80");
    }

    return make_pair(hp.substr(0, pos), hp.substr(pos+1));
 
 }

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

}} // ouinet::util namespace
