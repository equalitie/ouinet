#pragma once

#include <boost/beast/core/string.hpp>
#include "namespaces.h"

namespace ouinet {

/*
 * SplitString a string_view of comma separated values.
 * Usage:
 *
 * for (string_view v : SplitString("foo, bar ,, baz mag,", ',')) {
 *     cout << "\"" << v << "\"" << endl;
 * }
 *
 * // Output:
 * // "foo"
 * // "bar"
 * // ""
 * // "baz mag"
 * // ""
 */
class SplitString {
    using string_view = beast::string_view;

public:
    using value_type = string_view;

    struct const_iterator {
        string_view body;
        string_view rest;
        char separator;

        string_view operator*() const;
        const_iterator& operator++(); // prefix
        bool operator==(const_iterator) const;
        bool operator!=(const_iterator) const;
    };

    SplitString(beast::string_view body, char separator)
        : _body(body)
        , _separator(separator)
    {}

    const_iterator begin() const;
    const_iterator end() const;

private:
    static const_iterator split_first(string_view, char);

private:
    beast::string_view _body;
    char _separator;
};

inline void trim_whitespace(beast::string_view& v) {
    while (v.starts_with(' ')) v.remove_prefix(1);
    while (v.ends_with  (' ')) v.remove_suffix(1);
};

inline
SplitString::const_iterator SplitString::begin() const
{
    return split_first(_body, _separator);
}

inline
SplitString::const_iterator SplitString::end() const
{
    return const_iterator{ string_view(nullptr, 0)
                         , string_view(nullptr, 0)
                         , _separator};
}

inline
beast::string_view SplitString::const_iterator::operator*() const
{
    return body;
}

inline
SplitString::const_iterator& SplitString::const_iterator::operator++() // prefix
{
    auto i = split_first(rest, separator);
    *this = i;
    return *this;
}

inline
SplitString::const_iterator SplitString::split_first(string_view v, char separator)
{
    if (!v.data()) {
        return const_iterator{ string_view(nullptr, 0)
                             , string_view(nullptr, 0)
                             , separator};
    }

    auto pos = v.find(separator);
    
    if (pos == string_view::npos) {
        trim_whitespace(v);
        return const_iterator{v, string_view(nullptr, 0), separator};
    }
    
    auto body = v.substr(0, pos);
    auto rest = v.substr(pos + 1);

    trim_whitespace(body);

    return const_iterator{body, rest, separator};
}

inline
bool SplitString::const_iterator::operator==(const_iterator other) const
{
    // We need to use this because we don't consider string_view("")
    // to be equal with string_view(nullptr, 0).
    static const auto same = [](string_view v1, string_view v2) {
        return (v1.data() && v2.data())
             ? v1 == v2
             : v1.data() == v2.data();
    };

    return same(body, other.body) && same(rest, other.rest);
}

inline
bool SplitString::const_iterator::operator!=(const_iterator other) const
{
    return !(*this == other);
}

static inline
std::pair< beast::string_view
         , beast::string_view
         >
split_string_pair(beast::string_view v, char at) {
    using beast::string_view;

    auto at_pos = v.find(at);

    if (at_pos == string_view::npos) {
        trim_whitespace(v);
        return make_pair(v, string_view("", 0));
    }

    auto key = v.substr(0, at_pos);
    auto val = v.substr(at_pos + 1);

    trim_whitespace(key);
    trim_whitespace(val);

    return make_pair(key, val);
};

} // ouinet namespace
