#define BOOST_TEST_MODULE cache_control
#include <boost/test/included/unit_test.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/optional.hpp>

#include <cache_control.h>
#include <util.h>
#include <or_throw.h>
#include <iostream>

BOOST_AUTO_TEST_SUITE(ouinet_cache_control)

using namespace std;
using namespace ouinet;
namespace error = asio::error;
namespace posix_time = boost::posix_time;
using Entry    = CacheControl::CacheEntry;
using Request  = CacheControl::Request;
using Response = CacheControl::Response;
using posix_time::seconds;
using boost::optional;
using beast::string_view;
using ouinet::util::str;

static posix_time::ptime current_time() {
    return posix_time::second_clock::universal_time();
}

static optional<string_view> get(const Request& rq, http::field f)
{
    auto i = rq.find(f);
    if (i == rq.end()) return boost::none;
    return i->value();
}

template<class F> static void run_spawned(F&& f) {
    asio::io_service ios;
    asio::spawn(ios, forward<F>(f));
    ios.run();
}

BOOST_AUTO_TEST_CASE(test_cache_origin_fail)
{
    CacheControl cc;

    size_t cache_check = 0;
    size_t origin_check = 0;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        cache_check++;
        Response rs{http::status::ok, rq.version()};
        return Entry{current_time(), rs};
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        origin_check++;
        return or_throw<Response>(y, asio::error::connection_reset);
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            sys::error_code ec;
            auto rs = cc.fetch(req, yield[ec]);
            BOOST_CHECK(!ec);
            BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
        });

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_max_cached_age_old)
{
    CacheControl cc;

    size_t cache_check = 0;
    size_t origin_check = 0;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set( http::field::cache_control
              , str("max-age=", (cc.max_cached_age().seconds() + 10)));

        auto created = current_time()
                     - cc.max_cached_age()
                     - posix_time::seconds(5);

        return or_throw( y
                       , sys::error_code()
                       , Entry{created, rs});
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        origin_check++;
        BOOST_CHECK_EQUAL(rq.target(), "old");
        return or_throw<Response>(y, sys::error_code());
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "old", 11};
            sys::error_code ec;
            cc.fetch(req, yield[ec]);
            BOOST_CHECK(!ec);
        });

    BOOST_CHECK_EQUAL(cache_check, size_t(1));
    BOOST_CHECK_EQUAL(origin_check, size_t(1));
}

BOOST_AUTO_TEST_CASE(test_max_cached_age_new)
{
    CacheControl cc;

    size_t cache_check = 0;
    size_t origin_check = 0;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set( http::field::cache_control
              , str("max-age=", (cc.max_cached_age().seconds() + 10)));

        auto created = current_time()
                     - cc.max_cached_age()
                     + seconds(5);

        return or_throw( y
                       , sys::error_code()
                       , Entry{created, rs});
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        BOOST_CHECK(false);
        return or_throw<Response>(y, sys::error_code());
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "new", 11};
            sys::error_code ec;
            cc.fetch(req, yield[ec]);
            BOOST_CHECK(!ec);
        });

    BOOST_CHECK_EQUAL(cache_check, size_t(1));
    BOOST_CHECK_EQUAL(origin_check, size_t(0));
}

BOOST_AUTO_TEST_CASE(test_maxage)
{
    CacheControl cc;

    size_t cache_check = 0;
    size_t origin_check = 0;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set(http::field::cache_control, "max-age=60");

        auto created = current_time();

        if (rq.target() == "old") {
            created -= seconds(120);
        }
        else {
            created -= seconds(30);
            BOOST_CHECK(rq.target() == "new");
        }

        return or_throw( y
                       , sys::error_code()
                       , Entry{created, rs});
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        origin_check++;
        return or_throw<Response>(y, sys::error_code());
    };

    run_spawned([&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                sys::error_code ec;
                cc.fetch(req, yield[ec]);
                BOOST_CHECK(!ec);
            }
            {
                Request req{http::verb::get, "new", 11};
                sys::error_code ec;
                cc.fetch(req, yield[ec]);
                BOOST_CHECK(!ec);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, size_t(2));
    BOOST_CHECK_EQUAL(origin_check, size_t(1));
}

BOOST_AUTO_TEST_CASE(test_if_none_match)
{
    CacheControl cc;

    size_t cache_check = 0;
    size_t origin_check = 0;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set(http::field::cache_control, "max-age=10");
        rs.set(http::field::etag, "123");
        rs.set("X-Test", "from-cache");

        return Entry{current_time() + seconds(20), rs};
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        origin_check++;

        auto etag = get(rq, http::field::if_none_match);
        BOOST_REQUIRE(etag);

        Response rs{http::status::found, rq.version()};
        rs.set("X-Test", "from-origin");

        return or_throw(y, sys::error_code(), rs);
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "mypage", 11};
            sys::error_code ec;
            auto rs = cc.fetch(req, yield[ec]);
            BOOST_CHECK(!ec);

            BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
            BOOST_CHECK_EQUAL(rs["X-Test"], "from-cache");
        });

    BOOST_CHECK_EQUAL(cache_check, size_t(1));
    BOOST_CHECK_EQUAL(origin_check, size_t(1));
}

BOOST_AUTO_TEST_SUITE_END()
