#pragma once

#include <limits>
#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>

namespace ouinet { namespace parse {

//--------------------------------------------------------------------
namespace detail {
    inline
    bool is_digit(char c) { return '0' <= c && c <= '9'; }
}

//--------------------------------------------------------------------
template<class T>
std::enable_if_t< std::is_unsigned<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number(boost::string_view& s)
{
    size_t endpos = 0;

    while (endpos < s.size() && detail::is_digit(s[endpos])) {
        ++endpos;
    }

    if (endpos == 0) return boost::none;

    uint64_t r = 0;
    uint64_t m = 1;

    for (size_t i = 0; i < endpos; ++i) {
        unsigned c = (unsigned char) s[endpos-i-1];
        r += m * (c - '0');
        m *= 10;

        if (r > std::numeric_limits<T>::max()) {
            return boost::none;
        }
    }

    s.remove_prefix(endpos);
    return r;
}

//--------------------------------------------------------------------
template<class T>
std::enable_if_t< std::is_signed<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number(boost::string_view& s)
{
    auto s_ = s;

    if (s.empty()) return boost::none;

    int64_t sign = +1;

    if (s[0] == '+' || s[0] == '-') {
        if (s[0] == '-') sign = -1;
        s.remove_prefix(1);
    }

    auto on = number<uint64_t>(s);

    if (!on) {
        s = s_;
        return boost::none;
    }

    if (sign > 0) {
        if (*on > std::numeric_limits<T>::max()) {
            s = s_;
            return boost::none;
        }
        return *on;
    }
    else {
        if (*on > (uint64_t) -std::numeric_limits<T>::min()) {
            s = s_;
            return boost::none;
        }
        return -*on;
    }
}

//--------------------------------------------------------------------
}} // namespaces
