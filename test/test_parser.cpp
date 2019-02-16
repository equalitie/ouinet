#define BOOST_TEST_MODULE parser
#include <boost/test/included/unit_test.hpp>

#include "../src/parse/number.h"

BOOST_AUTO_TEST_SUITE(ouinet_parser)

using namespace std;
using namespace ouinet;
using string_view = boost::string_view;

BOOST_AUTO_TEST_CASE(test_unsigned_number) {
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

BOOST_AUTO_TEST_SUITE_END()


