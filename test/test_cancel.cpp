#define BOOST_TEST_MODULE cancel
#include <boost/test/unit_test.hpp>

#include <namespaces.h>
#include <optional>
#include "util/cancel.h"

using namespace ouinet;

BOOST_AUTO_TEST_CASE(cancel) {
    // sanity
    {
        Cancel c;
        BOOST_REQUIRE(!c);
        c();
        BOOST_REQUIRE(c);
    }

    // propagate downward
    {
        Cancel c0;
        Cancel c1(c0);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(c1);
    }

    // propagate downward - override root after
    {
        Cancel c0;
        Cancel c1(c0);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(c1);
        c0 = Cancel();
    }

    // don't propagate upward
    {
        Cancel c0;
        Cancel c1(c0);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        c1();
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(c1);
    }

    // destroy parent
    {
        std::optional<Cancel> c0 = Cancel();
        Cancel c1 = *c0;
        BOOST_REQUIRE(!*c0);
        BOOST_REQUIRE(!c1);
        c0.reset();
        BOOST_REQUIRE(!c1);
    }

    // move parent
    {
        Cancel c0;
        Cancel c1(c0);
        Cancel c2 = std::move(c0);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(!c2);
        c2();
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(c1);
        BOOST_REQUIRE(c2);
    }

    // move child
    {
        Cancel c0;
        Cancel c1(c0);
        Cancel c2 = std::move(c1);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(!c2);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(c2);
    }

    // move-destroy child
    {
        Cancel c0;
        Cancel c1(c0);
        { auto _ = std::move(c1); }
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(!c1);
    }

    // move middleman
    {
        Cancel c0;
        Cancel c1(c0);
        Cancel c2(c1);
        auto m = std::move(c1);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(!m);
        BOOST_REQUIRE(!c2);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(m);
        BOOST_REQUIRE(c2);
    }

    // move 2 middlemen
    {
        Cancel c0;
        Cancel c1(c0);
        Cancel c2(c1);
        Cancel c3(c2);
        auto m1 = std::move(c1);
        auto m2 = std::move(c2);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(!m1);
        BOOST_REQUIRE(!m2);
        BOOST_REQUIRE(!c2);
        BOOST_REQUIRE(!c3);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(!c2);
        BOOST_REQUIRE(m1);
        BOOST_REQUIRE(m2);
        BOOST_REQUIRE(c3);
    }

    // destroy middleman
    {
        Cancel c0;
        std::optional<Cancel> c1(c0);
        Cancel c2(*c1);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!*c1);
        BOOST_REQUIRE(!c2);
        c1.reset();
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(c2);
    }

    // destroy 2 middleman
    {
        Cancel c0;
        std::optional<Cancel> c1(c0);
        std::optional<Cancel> c2(*c1);
        Cancel c3(*c2);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!*c1);
        BOOST_REQUIRE(!*c2);
        BOOST_REQUIRE(!c3);
        c1.reset();
        c2.reset();
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(c3);
    }

    // destroy 2 middleman (reverse order)
    {
        Cancel c0;
        std::optional<Cancel> c1(c0);
        std::optional<Cancel> c2(*c1);
        Cancel c3(*c2);
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!*c1);
        BOOST_REQUIRE(!*c2);
        BOOST_REQUIRE(!c3);
        c2.reset();
        c1.reset();
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(c3);
    }

    // move-destroy middleman
    {
        Cancel c0;
        Cancel c1(c0);
        Cancel c2(c1);
        { auto m = std::move(c1); }
        BOOST_REQUIRE(!c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(!c2);
        c0();
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(!c1);
        BOOST_REQUIRE(c2);
    }

    // already cancelled parent
    {
        Cancel c0;
        c0();
        Cancel c1(c0);
        BOOST_REQUIRE(c0);
        BOOST_REQUIRE(c1);
    }
}

BOOST_AUTO_TEST_CASE(code) {
    {
        Cancel c;
        c();
        BOOST_REQUIRE_EQUAL(c.error_code(), asio::error::operation_aborted);
    }

    {
        Cancel c;
        c(asio::error::interrupted);
        BOOST_REQUIRE_EQUAL(c.error_code(), asio::error::interrupted);
    }

    {
        Cancel c0;
        Cancel c1(c0);
        c0();
        BOOST_REQUIRE_EQUAL(c1.error_code(), asio::error::operation_aborted);
    }

    // `operation_aborted` overwrites any other error code
    {
        Cancel c0;
        Cancel c1(c0);
        c1(asio::error::interrupted);
        c0();
        BOOST_REQUIRE_EQUAL(c0.error_code(), asio::error::operation_aborted);
        BOOST_REQUIRE_EQUAL(c1.error_code(), asio::error::operation_aborted);
    }

    {
        Cancel c0;
        Cancel c1(c0);
        c0();
        c1(asio::error::interrupted);
        BOOST_REQUIRE_EQUAL(c0.error_code(), asio::error::operation_aborted);
        BOOST_REQUIRE_EQUAL(c1.error_code(), asio::error::operation_aborted);
    }

    // `operation_aborted` does not propagate upwards
    {
        Cancel c0;
        Cancel c1(c0);
        c0(asio::error::interrupted);
        c1();
        BOOST_REQUIRE_EQUAL(c0.error_code(), asio::error::interrupted);
        BOOST_REQUIRE_EQUAL(c1.error_code(), asio::error::operation_aborted);
    }
}

BOOST_AUTO_TEST_CASE(connection) {
    Cancel c;
    bool cancelled = false;
    auto con = c.connect([&]() {
        cancelled = true;
    });
    BOOST_REQUIRE_EQUAL(c, false);
    BOOST_REQUIRE_EQUAL(cancelled, false);
    BOOST_REQUIRE_EQUAL(con, false);
    c();
    BOOST_REQUIRE_EQUAL(c, true);
    BOOST_REQUIRE_EQUAL(cancelled, true);
    BOOST_REQUIRE_EQUAL(con, true);
}
