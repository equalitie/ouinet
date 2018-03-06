#pragma once

#include "namespaces.h"

#include <unistd.h>  // for getpid()
#include <fstream>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/filesystem.hpp>
// Only available in Boost >= 1.64.0.
////#include <boost/process/environment.hpp>

namespace ouinet { namespace util {

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

inline
asio::ip::tcp::endpoint
parse_endpoint(const std::string& s, sys::error_code& ec)
{
    using namespace std;
    auto pos = s.find(':');

    ec = sys::error_code();

    if (pos == string::npos) {
        ec = asio::error::invalid_argument;
        return asio::ip::tcp::endpoint();
    }

    auto addr = asio::ip::address::from_string(s.substr(0, pos));

    auto pb = s.c_str() + pos + 1;
    uint16_t port = std::atoi(pb);

    if (port == 0 && !(*pb == '0' && *(pb+1) == 0)) {
        ec = asio::error::invalid_argument;
        return asio::ip::tcp::endpoint();
    }

    return asio::ip::tcp::endpoint(move(addr), port);
}

inline
asio::ip::tcp::endpoint
parse_endpoint(const std::string& s)
{
    sys::error_code ec;
    auto ep = parse_endpoint(s, ec);
    if (ec) throw sys::system_error(ec);
    return ep;
}

///////////////////////////////////////////////////////////////////////////////
namespace detail {
inline
std::string str_impl(std::stringstream& ss) {
    return ss.str();
}

template<class Arg, class... Args>
inline
std::string str_impl(std::stringstream& ss, Arg&& arg, Args&&... args) {
    ss << arg;
    return str_impl(ss, std::forward<Args>(args)...);
}

} // detail namespace

template<class... Args>
inline
std::string str(Args&&... args) {
    std::stringstream ss;
    return detail::str_impl(ss, std::forward<Args>(args)...);
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
// Write a small file at the given `path` with a `line` of content.
// If existing, truncate it.
inline
void create_state_file(const boost::filesystem::path& path, const std::string& line) {
    std::fstream fs(path.native(), std::fstream::out | std::fstream::trunc);
    fs << line << std::endl;
    fs.close();
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
// Remove all fields that are not listed in `keep_fields`.
template<class Message, class... Fields>
static Message filter_fields(const Message& message, const Fields&... keep_fields)
{
    // TODO: Instead creating a copy of the headers here, use the
    // erase function that uses iterators (for efficiency). It was broken
    // in Boost.Beast but should be fixed in new versions.
    auto copy = message;

    for (auto& f : message) {
        if (!field_is_one_of(f, keep_fields...)) {
            copy.erase(f.name_string());
        }
    }

    return copy;
}

///////////////////////////////////////////////////////////////////////////////
class PidFile {
    public:
        PidFile(const boost::filesystem::path& path) : pid_path(path) {
            // Only available in Boost >= 1.64.0.
            ////auto pid = boost::this_process::get_pid();
            // TODO: Check if this works under Windows (it is declared obsolete).
            auto pid = ::getpid();
            create_state_file(pid_path, std::to_string(pid));
        }

        ~PidFile() {
            try {
                remove(pid_path);
            } catch (...) {
            }
        }
    private:
        boost::filesystem::path pid_path;
};

///////////////////////////////////////////////////////////////////////////////

}} // ouinet::util namespace
