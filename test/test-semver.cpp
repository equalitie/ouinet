#define BOOST_TEST_MODULE semver

#include <boost/test/included/unit_test.hpp>
#include <iostream>
#include <string>

#include "semver.h"

#include <boost/optional/optional_io.hpp>

BOOST_AUTO_TEST_SUITE(semver)

using namespace std;
using namespace ouinet;
using boost::optional;


BOOST_AUTO_TEST_CASE(test_semver)
{
    BOOST_REQUIRE_EQUAL(Semver::parse("1.0.0"), Semver(1,0,0));
    BOOST_REQUIRE_EQUAL(Semver::parse("1.2.3"), Semver(1,2,3));
    BOOST_REQUIRE_EQUAL(Semver::parse(" 1.2.3"), Semver(1,2,3));
    BOOST_REQUIRE_EQUAL(Semver::parse(" 1.2.3 "), Semver(1,2,3));
    BOOST_REQUIRE_EQUAL(Semver::parse("1.2.3-a"), Semver(1,2,3, "a"));
    BOOST_REQUIRE_EQUAL(Semver::parse("1.2.3-a.b"), Semver(1,2,3, "a.b"));
    BOOST_REQUIRE_EQUAL(Semver::parse("1.2.3-a.b+c.d"), Semver(1,2,3, "a.b", "c.d"));
    //BOOST_REQUIRE_EQUAL(Semver::parse("1.2.3-+"), Semver(1,2,3));
    //BOOST_REQUIRE_EQUAL(Semver::parse("1.2.3+"), Semver(1,2,3));
}

BOOST_AUTO_TEST_SUITE_END()
