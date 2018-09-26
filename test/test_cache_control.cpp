#define BOOST_TEST_MODULE cache_control
#include <boost/test/included/unit_test.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/io_service.hpp>
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
    asio::spawn(ios, [&ios, f = forward<F>(f)](auto yield) {
            try {
                f(Yield(ios, yield));
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });
    ios.run();
}

BOOST_AUTO_TEST_CASE(test_parse_date)
{
    const auto p = [](const char* s) {
        auto date = CacheControl::parse_date(s);
        stringstream ss;
        ss << date;
        return ss.str();
    };

    // https://tools.ietf.org/html/rfc7234#section-5.3
    BOOST_CHECK_EQUAL(p("Sun, 06 Nov 1994 08:49:37 GMT"),   "1994-Nov-06 08:49:37");
    BOOST_CHECK_EQUAL(p("\" Sun, 06 Nov 1994 08:49:37 GMT"),"1994-Nov-06 08:49:37");
    BOOST_CHECK_EQUAL(p("Sunday, 06-Nov-94 08:49:37 GMT"),  "2094-Nov-06 08:49:37");
    BOOST_CHECK_EQUAL(p(" Sunday, 06-Nov-94 08:49:37 GMT"), "2094-Nov-06 08:49:37");
}

BOOST_AUTO_TEST_CASE(test_cache_origin_fail)
{
    CacheControl cc("test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        cache_check++;
        Response rs{http::status::ok, rq.version()};
        return Entry{current_time(), rs};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        return or_throw<Response>(y, asio::error::connection_reset);
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            auto rs = cc.fetch(req, yield);
            BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
        });

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_max_cached_age)
{
    CacheControl cc("test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set( http::field::cache_control
              , str("max-age=", (cc.max_cached_age().total_seconds() + 10)));

        auto created = current_time() - cc.max_cached_age();

        if (rq.target() == "old") created -= seconds(5);
        else                      created += seconds(5);

        return Entry{created, rs};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        BOOST_CHECK_EQUAL(rq.target(), "old");
        return Response{http::status::ok, rq.version()};
    };

    run_spawned([&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                cc.fetch(req, yield);
            }
            {
                Request req{http::verb::get, "new", 11};
                cc.fetch(req, yield);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_maxage)
{
    CacheControl cc("test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
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

        return Entry{created, rs};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        Response rs{http::status::ok, rq.version()};
        return rs;
    };

    run_spawned([&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                cc.fetch(req, yield);
            }
            {
                Request req{http::verb::get, "new", 11};
                cc.fetch(req, yield);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_http10_expires)
{
    CacheControl cc("test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    const auto format_time = [](posix_time::ptime t) {
        using namespace boost::posix_time;
        static const locale loc( locale::classic()
                               , new time_facet("%a, %d %b %Y %H:%M:%S"));

        stringstream ss;
        ss.imbue(loc);
        ss << t;
        return ss.str();
    };

    cc.fetch_stored = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};

        auto created = current_time();

        if (rq.target() == "old") {
            rs.set( http::field::expires
                  , format_time(current_time() - posix_time::seconds(10)));
        }
        else {
            BOOST_CHECK(rq.target() == "new");
            rs.set( http::field::expires
                  , format_time(current_time() + posix_time::seconds(10)));
        }

        return Entry{created, rs};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        Response rs{http::status::ok, rq.version()};
        return rs;
    };

    run_spawned([&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                cc.fetch(req, yield);
            }
            {
                Request req{http::verb::get, "new", 11};
                cc.fetch(req, yield);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_dont_load_cache_when_If_None_Match)
{
    CacheControl cc("test");

    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        BOOST_ERROR("Shouldn't go to cache");
        return Entry{current_time(), Response{}};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        Response rs{http::status::ok, rq.version()};
        rs.set("X-Test", "from-origin");
        return rs;
    };

    run_spawned([&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            req.set(http::field::if_none_match, "abc");
            auto rs = cc.fetch(req, yield);
            BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
        });

    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_no_etag_override)
{
    CacheControl cc("test");

    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        BOOST_ERROR("Shouldn't go to cache");
        return Entry{current_time(), Response{}};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;

        auto etag = get(rq, http::field::if_none_match);
        BOOST_CHECK(etag);
        BOOST_CHECK_EQUAL(*etag, "origin-etag");

        return Response{http::status::ok, rq.version()};
    };

    run_spawned([&](auto yield) {
            // In this test, the user agent provides its own etag.
            Request rq{http::verb::get, "mypage", 11};
            rq.set(http::field::if_none_match, "origin-etag");
            cc.fetch(rq, yield);
        });

    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_request_no_store)
{
    CacheControl cc("test");

    unsigned origin_check = 0;

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        return Response{http::status::ok, rq.version()};
    };

    cc.store = [&](auto rq, auto rs) {
        BOOST_ERROR("Shouldn't store");
    };

    run_spawned([&](auto yield) {
            Request rq{http::verb::get, "mypage", 11};
            rq.set(http::field::cache_control, "no-store");
            cc.fetch(rq, yield);
        });

    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_if_none_match)
{
    CacheControl cc("test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set(http::field::cache_control, "max-age=10");
        rs.set(http::field::etag, "123");
        rs.set("X-Test", "from-cache");

        return Entry{current_time() - seconds(20), rs};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;

        auto etag = get(rq, http::field::if_none_match);
        BOOST_REQUIRE(etag);

        if (*etag == "123") {
            Response rs{http::status::not_modified, rq.version()};
            rs.set("X-Test", "from-origin-not-modified");
            return rs;
        }

        Response rs{http::status::ok, rq.version()};
        rs.set("X-Test", "from-origin-ok");

        return rs;
    };

    run_spawned([&](auto yield) {
            {
                Request rq{http::verb::get, "mypage", 11};
                auto rs = cc.fetch(rq, yield);
                BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
                BOOST_CHECK_EQUAL(rs["X-Test"], "from-cache");
            }

            {
                // In this test, the user agent provides its own etag.
                Request rq{http::verb::get, "mypage", 11};
                rq.set(http::field::if_none_match, "abc");
                auto rs = cc.fetch(rq, yield);
                BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
                BOOST_CHECK_EQUAL(rs["X-Test"], "from-origin-ok");
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 2u);
}

BOOST_AUTO_TEST_CASE(test_req_no_cache_fresh_origin_ok)
{
    CacheControl cc("test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        cache_check++;
        Response rs{http::status::ok, rq.version()};
        // Return a fresh cached version.
        rs.set(http::field::cache_control, "max-age=3600");
        rs.set("X-Test", "from-cache");
        return Entry{current_time(), rs};
    };

    cc.fetch_fresh = [&](auto rq, auto y) {
        origin_check++;
        // Force using version from origin instead of validated version from cache
        // (i.e. not returning "304 Not Modified" here).
        Response rs{http::status::ok, rq.version()};
        rs.set("X-Test", "from-origin");
        return rs;
    };

    run_spawned([&](auto yield) {
            {
                // Cached resources requested without "no-cache" should come from the cache
                // since the cached version is fresh enough.
                Request req{http::verb::get, "foo", 11};
                auto rs = cc.fetch(req, yield);
                BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
                BOOST_CHECK_EQUAL(rs["X-Test"], "from-cache");
            }
            {
                // Cached resources requested without "no-cache" should come from or be validated by the origin.
                // In this test we know it will be the origin.
                Request req{http::verb::get, "foo", 11};
                req.set(http::field::cache_control, "no-cache");
                auto rs = cc.fetch(req, yield);
                BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
                BOOST_CHECK_EQUAL(rs["X-Test"], "from-origin");
            }
        });

    // Cache should have been checked without "no-cache",
    // it may or may not have been checked with "no-cache".
    BOOST_CHECK(1u <= cache_check && cache_check < 3u);
    // Origin should have only been checked with "no-cache".
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_SUITE_END()
