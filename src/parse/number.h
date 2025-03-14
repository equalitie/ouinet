#pragma once

#include <limits>
#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>
#include "../util.h"

namespace ouinet { namespace parse {

//--------------------------------------------------------------------
namespace detail {
    inline
    bool is_digit(char c) { return '0' <= c && c <= '9'; }

    inline
    uint8_t digit(char c) { return c - '0'; }

    template<typename T> struct MaxStr;
    template<> struct MaxStr<uint8_t>  { boost::string_view str() { return boost::string_view("255", 3); } };
    template<> struct MaxStr<uint16_t> { boost::string_view str() { return boost::string_view("65535", 5); } };
    template<> struct MaxStr<uint32_t> { boost::string_view str() { return boost::string_view("4294967295", 10); } };
    template<> struct MaxStr<uint64_t> { boost::string_view str() { return boost::string_view("18446744073709551615", 20); } };
}

//--------------------------------------------------------------------
template<class StoreT, class T>
std::enable_if_t< std::is_unsigned<StoreT>::value && std::is_integral<StoreT>::value
                && std::is_unsigned<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number_with(boost::string_view& s)
{
    static_assert(std::numeric_limits<T>::max() <= std::numeric_limits<StoreT>::max());

    size_t endpos = 0;

    while (endpos < s.size() && detail::is_digit(s[endpos])) {
        ++endpos;
    }

    if (endpos == 0) return boost::none;

    auto max_str = detail::MaxStr<T>().str();

    if (endpos > max_str.size()) {
        return boost::none;
    }

    if (endpos == max_str.size()) {
        for (size_t i = 0; i <= endpos; ++i) {
            auto d_in  = detail::digit(s[i]);
            auto d_max = detail::digit(max_str[i]);

            if (d_in > d_max) {
                return boost::none;
            }

            if (d_in < d_max) {
                break;
            }
        }
    }

    StoreT r = 0;
    StoreT m = 1;

    for (size_t i = 0; i < endpos; ++i) {
        uint8_t d = detail::digit(s[endpos-i-1]);

        r += m * d;
        m *= 10;
    }

    s.remove_prefix(endpos);
    return r;
}

template<class T>
std::enable_if_t< std::is_unsigned<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number(boost::string_view& s)
{
    return number_with<uint64_t, T>(s);
}

// Since some boost version the `beast::string_view` became a different type
// than `boost::string_view`.  TODO: Find the exact version where that changed
// or just stop supporting old boost versions.
#if BOOST_VERSION >= 108000
template<class T>
std::enable_if_t< std::is_unsigned<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number(beast::string_view& s)
{
    auto bs = util::to_boost(s);
    auto ret = number<T>(bs);
    s = util::to_beast(bs);
    return ret;
}
#endif

//--------------------------------------------------------------------
template<class T>
std::enable_if_t< std::is_signed<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number(boost::string_view& s)
{
    auto s_ = s;

    if (s.empty()) return boost::none;

    bool positive = true;

    if (s[0] == '+' || s[0] == '-') {
        if (s[0] == '-') positive = false;
        s.remove_prefix(1);
    }

    // Min is always less by one than (-1 * max)
    static_assert(-(std::numeric_limits<T>::min() + 1) == std::numeric_limits<T>::max());

    // Check that uint64_t is enough to temporarily store the negative value of the parsed string.
    // The check for the positive value is done inside the unsigned `number` parser function.
    static_assert(-(std::numeric_limits<T>::min() + 1) <= std::numeric_limits<uint64_t>::max());

    auto on = number<uint64_t>(s);

    if (!on) {
        s = s_;
        return boost::none;
    }

    auto n = *on;

    if (positive) {
        if (n > std::numeric_limits<T>::max()) {
            s = s_;
            return boost::none;
        }
        return n;
    }
    else {
        if ((n - 1) == std::numeric_limits<T>::max()) {
            return -n;
        }
        if (n > (uint64_t) -(std::numeric_limits<T>::min() + 1)) {
            s = s_;
            return boost::none;
        }
        return -n;
    }
}

//--------------------------------------------------------------------
}} // namespaces
