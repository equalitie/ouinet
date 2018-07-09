#include "bencoding.h"

namespace ouinet {
namespace bittorrent {

struct BencodedValueVisitor : public boost::static_visitor<std::string> {
    std::string operator()(const int64_t& value) {
        return std::string("i") + std::to_string(value) + std::string("e");
    }

    std::string operator()(const std::string& value) {
        return std::to_string(value.size()) + std::string(":") + value;
    }

    std::string operator()(const BencodedList& value) {
        std::string output = "l";
        for (const auto& item : value) {
            output += boost::apply_visitor(*this, item);
        }
        output += "e";
        return output;
    }

    std::string operator()(const BencodedMap& value) {
        std::string output = "d";
        for (const auto& item : value) {
            output += (*this)(item.first);
            output += boost::apply_visitor(*this, item.second);
        }
        output += "e";
        return output;
    }
};

std::string bencoding_encode(const BencodedValue& value)
{
    BencodedValueVisitor visitor;
    return boost::apply_visitor(visitor, value);
}



boost::optional<int64_t> destructive_parse_int(std::string& encoded)
{
    try {
        std::size_t pos;
        int64_t value = std::stoll(encoded, &pos, 10);
        encoded.erase(0, pos);
        return value;
    } catch(...) {
        return boost::none;
    }
}

boost::optional<std::string> destructive_parse_string(std::string& encoded)
{
    boost::optional<int64_t> size = destructive_parse_int(encoded);
    if (!size) {
        return boost::none;
    }
    if (encoded[0] != ':') {
        return boost::none;
    }
    encoded.erase(0, 1);
    if (encoded.size() < (size_t)*size) {
        return boost::none;
    }
    std::string value = encoded.substr(0, *size);
    encoded.erase(0, *size);
    return value;
}

boost::optional<BencodedValue> destructive_parse_value(std::string& encoded)
{
    if (encoded.size() == 0) {
        return boost::none;
    }

    if (encoded[0] == 'i') {
        encoded.erase(0, 1);
        boost::optional<int64_t> value = destructive_parse_int(encoded);
        if (!value) {
            return boost::none;
        }
        if (encoded.size() == 0) {
            return boost::none;
        }
        if (encoded[0] != 'e') {
            return boost::none;
        }
        encoded.erase(0, 1);
        return BencodedValue(*value);
    } else if ('0' <= encoded[0] && encoded[0] <= '9') {
        boost::optional<std::string> value = destructive_parse_string(encoded);
        if (!value) {
            return boost::none;
        }
        return BencodedValue(std::move(*value));
    } else if (encoded[0] == 'l') {
        encoded.erase(0, 1);
        BencodedList output;
        while (encoded.size() > 0 && encoded[0] != 'e') {
            boost::optional<BencodedValue> value = destructive_parse_value(encoded);
            if (!value) {
                return boost::none;
            }
            output.push_back(std::move(*value));
        }
        if (encoded.size() == 0) {
            return boost::none;
        }
        assert(encoded[0] == 'e');
        encoded.erase(0, 1);
        return BencodedValue(output);
    } else if (encoded[0] == 'd') {
        encoded.erase(0, 1);
        BencodedMap output;
        while (encoded.size() > 0 && encoded[0] != 'e') {
            boost::optional<std::string> key = destructive_parse_string(encoded);
            if (!key) {
                return boost::none;
            }
            boost::optional<BencodedValue> value = destructive_parse_value(encoded);
            if (!value) {
                return boost::none;
            }
            output[std::move(*key)] = std::move(*value);
        }
        if (encoded.size() == 0) {
            return boost::none;
        }
        assert(encoded[0] == 'e');
        encoded.erase(0, 1);
        return BencodedValue(output);
    } else {
        return boost::none;
    }
}

boost::optional<BencodedValue> bencoding_decode(std::string encoded)
{
    return destructive_parse_value(encoded);
}

} // bittorrent namespace
} // ouinet namespace
