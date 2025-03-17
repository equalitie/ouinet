#define BOOST_TEST_MODULE parser
#include <boost/test/included/unit_test.hpp>
#include <boost/optional/optional_io.hpp>

#include "../src/parse/number.h"

BOOST_AUTO_TEST_SUITE(ouinet_parser)

using namespace std;
using namespace ouinet;
using string_view = boost::string_view;

BOOST_AUTO_TEST_CASE(test_unsigned_number) {
    {
        stringstream ss;
        ss << (uint64_t) numeric_limits<uint8_t>::max();
        BOOST_REQUIRE_EQUAL(ss.str(), parse::detail::MaxStr<1>().str());
    }

    {
        stringstream ss;
        ss << (uint64_t) numeric_limits<uint16_t>::max();
        BOOST_REQUIRE_EQUAL(ss.str(), parse::detail::MaxStr<2>().str());
    }

    {
        stringstream ss;
        ss << (uint64_t) numeric_limits<uint32_t>::max();
        BOOST_REQUIRE_EQUAL(ss.str(), parse::detail::MaxStr<4>().str());
    }

    {
        stringstream ss;
        ss << (uint64_t) numeric_limits<uint64_t>::max();
        BOOST_REQUIRE_EQUAL(ss.str(), parse::detail::MaxStr<8>().str());
    }

    {
        string_view s = "0";
        auto on = parse::number<unsigned>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(0u, *on);
    }

    {
        string_view s = "1234";
        auto on = parse::number<unsigned>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(1234u, *on);
    }

    {
        string_view s = "01234";
        auto on = parse::number<unsigned>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(1234u, *on);
    }

    {
        string_view s = "+01234";
        string_view s_ = s;
        BOOST_REQUIRE(!parse::number<unsigned>(s));
        // Don't consume if parse fails
        BOOST_REQUIRE_EQUAL(s.size(), s_.size());
    }

    {
        string_view s = "-01234";
        BOOST_REQUIRE(!parse::number<unsigned>(s));
    }

    {
        // Max uint8_t
        string_view s = "255";
        auto on = parse::number<uint8_t>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(255, *on);
    }

    {
        // Too big
        string_view s = "256";
        BOOST_REQUIRE(!parse::number<uint8_t>(s));
    }
}

BOOST_AUTO_TEST_CASE(test_signed_number) {
    {
        string_view s = "1234";
        auto on = parse::number<int>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(1234, *on);
    }

    {
        string_view s = "+1234";
        auto on = parse::number<int>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(1234, *on);
    }

    {
        string_view s = "-1234";
        auto on = parse::number<int>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(-1234, *on);
    }

    {
        string_view s = "-0";
        auto on = parse::number<int>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(0, *on);
    }

    {
        string_view s = "01234";
        auto on = parse::number<int>(s);

        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(1234, *on);
    }

    {
        string_view s = "+01234";
        auto on = parse::number<int>(s);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(1234, *on);
    }

    {
        string_view s = "+a";
        auto on = parse::number<int>(s);
        BOOST_REQUIRE(!on);
        // Don't consume if not parsed
        BOOST_REQUIRE_EQUAL(s.size(), 2u);
    }

    {
        string_view s = "-01234";
        auto on = parse::number<int>(s);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(-1234, *on);
    }

    {
        // Max char
        string_view s = "127";
        auto on = parse::number<char>(s);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(127, *on);
    }

    {
        // Min char
        string_view s = "-128";
        auto on = parse::number<char>(s);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(-128, *on);
    }

    {
        // Too big
        string_view s = "128";
        BOOST_REQUIRE(!parse::number<char>(s));
    }

    {
        // Too small
        string_view s = "-129";
        BOOST_REQUIRE(!parse::number<char>(s));
    }
}

BOOST_AUTO_TEST_CASE(test_overflows) {
    {
        stringstream ss;
        ss << numeric_limits<uint64_t>::max();
        auto str = ss.str();
        string_view sv(str.data(), str.size());
        auto on = parse::number<uint64_t>(sv);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(numeric_limits<uint64_t>::max(), *on);
    }

    {
        stringstream ss;
        ss << numeric_limits<int64_t>::max();
        auto str = ss.str();
        string_view sv(str.data(), str.size());
        auto on = parse::number<int64_t>(sv);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(numeric_limits<int64_t>::max(), *on);
    }

    {
        stringstream ss;
        ss << numeric_limits<int64_t>::min();
        auto str = ss.str();
        string_view sv(str.data(), str.size());
        auto on = parse::number<int64_t>(sv);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(numeric_limits<int64_t>::min(), *on);
    }

    // Check for overflows over the max storage
    for (uint32_t i = 0; i < 2 * numeric_limits<uint8_t>::max(); ++i) {
        stringstream ss;
        ss << i;
        auto str = ss.str();
        string_view sv(str.data(), str.size());
        auto on = parse::number<uint8_t>(sv);
        if (i <= numeric_limits<uint8_t>::max()) {
            BOOST_REQUIRE(on);
            BOOST_REQUIRE_EQUAL(i, *on);
        }
        else {
            BOOST_REQUIRE(!on);
        }
    }

    // There can be any number of preceding zeros, even if the length of the
    // string is longer than stringified version of the max value of the given
    // type.
    {
        string_view s = "0255";
        auto on = parse::number<uint8_t>(s);
        BOOST_REQUIRE(on);
        BOOST_REQUIRE_EQUAL(255, *on);
    }
}

BOOST_AUTO_TEST_SUITE_END()


