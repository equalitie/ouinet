#pragma once

#include <array>
#include <string>
#include <vector>

namespace ouinet {
namespace util {
namespace bytes {

template<class B> struct is_byte_type { static const bool value = false; };
template<class S> struct is_bytestring_type { static const bool value = false; };

template<> struct is_byte_type<char> { static const bool value = true; };
template<> struct is_byte_type<signed char> { static const bool value = true; };
template<> struct is_byte_type<unsigned char> { static const bool value = true; };

template<> struct is_bytestring_type<std::string> { static const bool value = true; };
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

inline std::string from_hex(const std::string& hex)
{
    std::string output;
    for (unsigned int i = 0; i * 2 < hex.size(); i++) {
        output += (unsigned char)std::stoi(hex.substr(2 * i, 2), nullptr, 16);
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

} // bytes namespace
} // util namespace
} // ouinet namespace
