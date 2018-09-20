#pragma once

#include "namespaces.h"
#include <unistd.h>  // for getpid()
#include <fstream>
#include <string>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast/http/fields.hpp>

namespace ouinet {

static const std::string http_header_prefix = "X-Ouinet-";

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
// keep Ouinet internal headers if `keep_ouinet` is true.
template<class Message, class... Fields>
static Message filter_fields(const Message& message, bool keep_ouinet, const Fields&... keep_fields)
{
    // TODO: Instead creating a copy of the headers here, use the
    // erase function that uses iterators (for efficiency). It was broken
    // in Boost.Beast but should be fixed in new versions.
    auto copy = message;

    for (auto& f : message) {
        if (!( field_is_one_of(f, keep_fields...)  // TODO: do case insensitive cmp
               || (keep_ouinet && f.name_string().starts_with(http_header_prefix)))) {
            copy.erase(f.name_string());
        }
    }

    return copy;
}

}} // ouinet::util namespace
