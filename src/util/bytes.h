#pragma once

#include <array>
#include <string>
#include <vector>
#include <boost/utility/string_view.hpp>
#include <boost/optional.hpp>

namespace ouinet {
namespace util {
namespace bytes {

template<class B> struct is_byte_type { static const bool value = false; };
template<class S> struct is_bytestring_type { static const bool value = false; };

template<> struct is_byte_type<char> { static const bool value = true; };
template<> struct is_byte_type<signed char> { static const bool value = true; };
template<> struct is_byte_type<unsigned char> { static const bool value = true; };

template<> struct is_bytestring_type<std::string> { static const bool value = true; };
template<> struct is_bytestring_type<boost::string_view> { static const bool value = true; };
template<class B> struct is_bytestring_type<std::vector<B>> { static const bool value = is_byte_type<B>::value; };
template<std::size_t N, class B> struct is_bytestring_type<std::array<B, N>> { static const bool value = is_byte_type<B>::value; };

/*
 * Conversions between different bytestring types.
 */

template<class S> std::string to_string(const S& bytestring)
{
    static_assert(is_bytestring_type<S>::value, "Not a bytestring type");
    return std::string(reinterpret_cast<const char *>(bytestring.data()), bytestring.size());
}

template<class S> boost::string_view to_string_view(const S& bytestring)
{
    static_assert(is_bytestring_type<S>::value, "Not a bytestring type");
    return boost::string_view(reinterpret_cast<const char *>(bytestring.data()), bytestring.size());
}

template<class B, class S> std::vector<B> to_vector(const S& bytestring)
{
    static_assert(is_byte_type<B>::value, "Not a byte type");
    static_assert(is_bytestring_type<S>::value, "Not a bytestring type");
    return std::vector<B>(
        reinterpret_cast<const B *>(bytestring.data()),
        reinterpret_cast<const B *>(bytestring.data()) + bytestring.size()
    );
}

template<class B, std::size_t N, class S> std::array<B, N> to_array(const S& bytestring)
{
    static_assert(is_byte_type<B>::value, "Not a byte type");
    static_assert(is_bytestring_type<S>::value, "Not a bytestring type");
    assert(bytestring.size() == N);
    std::array<B, N> output;
    std::copy(
        reinterpret_cast<const B *>(bytestring.data()),
        reinterpret_cast<const B *>(bytestring.data()) + bytestring.size(),
        output.begin()
    );
    return output;
}

inline bool is_hex(boost::string_view s)
{
    static const std::string hex_chars = "0123456789abcdefABCDEF";

    for (size_t i = 0; i < s.size(); i++) {
        if (hex_chars.find(s[i]) == std::string::npos) {
            return false;
        }
    }

    return true;
}

template<class S> std::string to_hex(const S& bytestring)
{
    static_assert(is_bytestring_type<S>::value, "Not a bytestring type");
    std::string output;
    for (unsigned int i = 0; i < bytestring.size(); i++) {
        unsigned char c = bytestring.data()[i];
        const char* digits = "0123456789abcdef";
        output += digits[(c >> 4) & 0xf];
        output += digits[(c >> 0) & 0xf];
    }
    return output;
}

inline
boost::optional<unsigned char> from_hex(char c)
{
    if ('0' <= c && c <= '9') {
        return c - '0';
    } else if ('a' <= c && c <= 'f') {
        return 10 + c - 'a';
    } else if ('A' <= c && c <= 'F') {
        return 10 + c - 'A';
    } else return boost::none;
}

inline
boost::optional<unsigned char> from_hex(char c1, char c2)
{
    auto on1 = from_hex(c1);
    if (!on1) return boost::none;
    auto on2 = from_hex(c2);
    if (!on2) return boost::none;
    return *on1*16+*on2;
}

inline boost::optional<std::string> from_hex(boost::string_view hex)
{
    std::string output((hex.size() >> 1) + (hex.size() & 1), '\0');

    size_t i = 0;
    while (size_t s = hex.size()) {
        boost::optional<unsigned char> oc;

        if (s == 1) { oc = from_hex(hex[0]);         hex.remove_prefix(1); }
        else        { oc = from_hex(hex[0], hex[1]); hex.remove_prefix(2); }

        if (!oc) return boost::none;

        output[i++] = *oc;
    }

    return output;
}

template<class S> std::string to_printable(const S& bytestring)
{
    static_assert(is_bytestring_type<S>::value, "Not a bytestring type");
    std::string output;
    for (unsigned int i = 0; i < bytestring.size(); i++) {
        unsigned char c = bytestring.data()[i];
        if (c == '\\' || c == '"') {
            output += '\\';
            output += c;
        } else if (' ' <= c && c <= '~') {
            output += c;
        } else {
            const char* digits = "0123456789abcdef";
            output += "\\x";
            output += digits[(c >> 4) & 0xf];
            output += digits[(c >> 0) & 0xf];
        }
    }
    return output;
}

inline
boost::optional<std::string> from_printable(boost::string_view s)
{
    std::string output;

    if (s.size() < 2) return boost::none;
    if (s.front() != '"') return boost::none;
    if (s.back() != '"') return boost::none;

    s.remove_prefix(1);
    s.remove_suffix(1);

    while (!s.empty()) {
        if (s.substr(0, 2) == "\\x") {
            s.remove_prefix(2);
            if (s.size() < 2) return boost::none;
            auto oc = from_hex(s[0], s[1]);
            if (!oc) return boost::none;
            s.remove_prefix(2);
            output.push_back(*oc);
        } else {
            output.push_back(s[0]);
            s.remove_prefix(1);
        }
    }

    return output;
}

} // bytes namespace
} // util namespace
} // ouinet namespace
