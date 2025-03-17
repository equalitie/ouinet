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

    template<size_t byte_count> struct MaxStr;
    template<> struct MaxStr<1> { boost::string_view str() { return boost::string_view("255", 3); } };
    template<> struct MaxStr<2> { boost::string_view str() { return boost::string_view("65535", 5); } };
    template<> struct MaxStr<4> { boost::string_view str() { return boost::string_view("4294967295", 10); } };
    template<> struct MaxStr<8> { boost::string_view str() { return boost::string_view("18446744073709551615", 20); } };

    // Might be worth considering using boost::type_traits::make_unsigned,
    // thoug we don't need that genericity and this may compile faster.
    template<size_t byte_count> struct Unsigned;
    template<> struct Unsigned<1> { using type = uint8_t; };
    template<> struct Unsigned<2> { using type = uint16_t; };
    template<> struct Unsigned<4> { using type = uint32_t; };
    template<> struct Unsigned<8> { using type = uint64_t; };
}

//--------------------------------------------------------------------

template<class T>
std::enable_if_t< std::is_unsigned<T>::value && std::is_integral<T>::value
                , boost::optional<T>
                >
number(boost::string_view& s)
{
    auto s_ = s;

    bool zeros_stripped = false;

    // Strip leading zeros
    while (s.starts_with('0')) {
        zeros_stripped = true;
        s.remove_prefix(1);
    }

    size_t endpos = 0;

    while (endpos < s.size() && detail::is_digit(s[endpos])) {
        ++endpos;
    }

    if (endpos == 0) {
        if (zeros_stripped) {
            return 0;
        }
        s = s_;
        return boost::none;
    }

    auto max_str = detail::MaxStr<sizeof(T)>().str();

    // Check the parsed string will fit into T without overflow.
    if (endpos > max_str.size()) {
        s = s_;
        return boost::none;
    }

    // Still checking the above.
    if (endpos == max_str.size()) {
        for (size_t i = 0; i <= endpos; ++i) {
            auto d_in  = detail::digit(s[i]);
            auto d_max = detail::digit(max_str[i]);

            if (d_in > d_max) {
                s = s_;
                return boost::none;
            }

            if (d_in < d_max) {
                break;
            }
        }
    }

    T r = 0;
    T m = 1;

    for (size_t i = 0; i < endpos; ++i) {
        uint8_t d = detail::digit(s[endpos-i-1]);

        r += m * d;
        m *= 10;
    }

    s.remove_prefix(endpos);
    return r;
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
    using Abs = typename detail::Unsigned<sizeof(T)>::type;

    // Min is always less by one than (-1 * max)
    static_assert(-(std::numeric_limits<T>::min() + 1) == std::numeric_limits<T>::max());

    // Check that Abs is enough to temporarily store the absolute value of the
    // parsed string. Without considering overflows, the check would look like
    // (-T::min <= Abs::max).
    static_assert(-(std::numeric_limits<T>::min() + 1) < std::numeric_limits<Abs>::max());

    if (s.empty()) return boost::none;

    auto s_ = s;

    bool negative = false;

    if (s[0] == '+') {
        // Consider putting this check in `parse::number<uint*_t>`.
        s.remove_prefix(1);
    }
    else if (s[0] == '-') {
        negative = true;
        s.remove_prefix(1);
    }

    auto abs_opt = number<Abs>(s);

    if (!abs_opt) {
        s = s_;
        return boost::none;
    }

    Abs abs = *abs_opt;

    if (abs == 0) {
        // Regardless of the sign.
        return abs;
    }

    if (!negative) {
        if (abs > std::numeric_limits<T>::max()) {
            s = s_;
            return boost::none;
        }
        return abs;
    }
    else {
        // `abs` may be greater than T::max() by one when `negative` is `true`
        // (see the static assertions above). It is also strictly greater than
        // zero.
        if ((abs - 1) <= std::numeric_limits<T>::max()) {
            return -abs;
        }
        s = s_;
        return boost::none;
    }
}

//--------------------------------------------------------------------
}} // namespaces
