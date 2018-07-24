#define BOOST_TEST_MODULE btree
#include <boost/test/included/unit_test.hpp>

#include <util.h>

using namespace std;
using namespace ouinet::util;

BOOST_AUTO_TEST_SUITE(test_match_http_url)

BOOST_AUTO_TEST_CASE(test_1)
{
    {
        url_match m;
        bool r = match_http_url("http://example.com/?a=b", m);
        BOOST_REQUIRE(r);
        BOOST_REQUIRE_EQUAL(m.scheme, "http");
        BOOST_REQUIRE_EQUAL(m.host, "example.com");
        BOOST_REQUIRE_EQUAL(m.path, "/");
        BOOST_REQUIRE_EQUAL(m.query, "a=b");
        BOOST_REQUIRE_EQUAL(m.fragment, "");
    }
    {
        url_match m;
        bool r = match_http_url("https://example.com/#aaa", m);
        BOOST_REQUIRE(r);
        BOOST_REQUIRE_EQUAL(m.scheme, "https");
        BOOST_REQUIRE_EQUAL(m.host, "example.com");
        BOOST_REQUIRE_EQUAL(m.path, "/");
        BOOST_REQUIRE_EQUAL(m.query, "");
        BOOST_REQUIRE_EQUAL(m.fragment, "aaa");
    }
    {
        url_match m;
        bool r = match_http_url("/", m);
        BOOST_REQUIRE(r);
        BOOST_REQUIRE_EQUAL(m.scheme, "");
        BOOST_REQUIRE_EQUAL(m.host, "");
        BOOST_REQUIRE_EQUAL(m.path, "/");
        BOOST_REQUIRE_EQUAL(m.query, "");
        BOOST_REQUIRE_EQUAL(m.fragment, "");
    }
}

BOOST_AUTO_TEST_SUITE_END()

