#define BOOST_TEST_MODULE cache_control
#include <boost/test/included/unit_test.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <cache_control.h>
#include <namespaces.h>
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

static posix_time::ptime current_time() {
    return posix_time::second_clock::universal_time();
}

template<class F> static void run_spawned(F&& f) {
    asio::io_service ios;
    asio::spawn(ios, forward<F>(f));
    ios.run();
}

BOOST_AUTO_TEST_CASE(test_cache_no_origin)
{
    CacheControl cc;

    bool checked_cache = false;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        checked_cache = true;
        Response rs{http::status::ok, rq.version()};
        return or_throw( y
                       , sys::error_code()
                       , Entry{current_time(), rs});
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        BOOST_CHECK(false);
        return or_throw<Response>(y, sys::error_code());
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            sys::error_code ec;
            cc.fetch(req, yield[ec]);
            BOOST_CHECK(!ec);
        });

    BOOST_CHECK(checked_cache);
}

BOOST_AUTO_TEST_CASE(test_max_cached_age_old)
{
    CacheControl cc;

    size_t cache_check = 0;
    size_t origin_check = 0;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};

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

        auto created = current_time()
                     - cc.max_cached_age()
                     + posix_time::seconds(5);

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
            created -= posix_time::seconds(120);
        }
        else {
            created -= posix_time::seconds(30);
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

BOOST_AUTO_TEST_SUITE_END()
