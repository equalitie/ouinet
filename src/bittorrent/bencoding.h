#pragma once

#include <map>
#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/variant.hpp>
#include <boost/utility/string_view.hpp>

namespace ouinet {
namespace bittorrent {

/*
 * http://www.bittorrent.org/beps/bep_0003.html#bencoding
 */

class BencodedValue;

typedef std::vector<BencodedValue> BencodedList;
typedef std::map<std::string, BencodedValue> BencodedMap;

namespace detail {
    typedef boost::variant<
        int64_t,
        std::string,
        BencodedList,
        BencodedMap
    > value;
}

class BencodedValue : public detail::value {
    public:
    BencodedValue() : detail::value("") {}
    BencodedValue(int64_t value): detail::value(value) {}
    BencodedValue(const std::string& value): detail::value(value) {}
    BencodedValue(const char* value): detail::value(std::string(value)) {}
    BencodedValue(const BencodedList& value): detail::value(value) {}
    BencodedValue(const BencodedMap& value): detail::value(value) {}

    bool is_int() const { return boost::get<int64_t>(this) ? true : false; }
    bool is_string() const { return boost::get<std::string>(this) ? true : false; }
    bool is_list() const { return boost::get<BencodedList>(this) ? true : false; }
    bool is_map() const { return boost::get<BencodedMap>(this) ? true : false; }

    boost::optional<int64_t> as_int() const {
        auto v = boost::get<int64_t>(this);
        if (!v) return boost::none;
        return *v;
    }

    boost::optional<std::string> as_string() const {
        auto v = boost::get<std::string>(this);
        if (!v) return boost::none;
        return *v;
    }

    boost::optional<BencodedList> as_list() const {
        auto v = boost::get<BencodedList>(this);
        if (!v) return boost::none;
        return *v;
    }

    boost::optional<BencodedMap> as_map() const {
        auto v = boost::get<BencodedMap>(this);
        if (!v) return boost::none;
        return *v;
    }

    bool operator==(const char* str) const {
        auto opt_str = as_string();
        return opt_str && *opt_str == str;
    }

    bool operator!=(const char* str) const {
        return !(*this == str);
    }

    bool operator==(const std::string& str) const {
        auto opt_str = as_string();
        return opt_str && *opt_str == str;
    }

    bool operator!=(const std::string& str) const {
        return !(*this == str);
    }
};

std::string bencoding_encode(const BencodedValue& value);
boost::optional<BencodedValue> bencoding_decode(boost::string_view encoded);

std::ostream& operator<<(std::ostream&, const BencodedValue&);

} // bittorrent namespace
} // ouinet namespace
