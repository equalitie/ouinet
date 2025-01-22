#define BOOST_TEST_MODULE cache_control
#include <boost/test/included/unit_test.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/optional.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/connect_pipe.hpp>

#include <cache_control.h>
#include <http_util.h>
#include <util.h>
#include <or_throw.h>
#include <session.h>
#include <iostream>

BOOST_AUTO_TEST_SUITE(ouinet_cache_control)

using namespace std;
using namespace ouinet;
namespace error = asio::error;
namespace posix_time = boost::posix_time;
using Entry    = CacheEntry;
using Request  = CacheControl::Request;
using Response = CacheControl::Response;
using posix_time::seconds;
using boost::optional;
using beast::string_view;
using ouinet::util::str;
using OuinetYield = ouinet::Yield;

static const optional<string> dht_group("fake-dht-group");

static posix_time::ptime current_time() {
    return posix_time::second_clock::universal_time();
}

static optional<string_view> get(const Request& rq, http::field f)
{
    auto i = rq.find(f);
    if (i == rq.end()) return boost::none;
    return i->value();
}

template<class F> static void run_spawned(asio::io_context& ctx, F&& f) {
    asio::spawn(ctx, [&ctx, f = forward<F>(f)](auto yield) {
            try {
                f(OuinetYield(ctx, yield));
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });
    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_parse_date)
{
    const auto p = [](const char* s) {
        auto date = util::parse_date(s);
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

/*
 * This class implements an empty `async_write_some` method just to fulfill
 * `GenericStream` requirements when passing an Asio pipe instead of a stream
 * file object to the tests.
 */
class readable_pipe_patched : public asio::readable_pipe {
public:
    explicit readable_pipe_patched(asio::io_context &ctx) : asio::readable_pipe{ctx} {}

    template <typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(const ConstBufferSequence& buffer, WriteHandler handler) {}
};

struct Pipe {
    readable_pipe_patched source;
    asio::writable_pipe sink;
};

Pipe make_pipe(asio::io_context& ctx) {
    readable_pipe_patched p0{ctx};
    asio::writable_pipe p1{ctx};
    asio::connect_pipe(p0, p1);
    return {std::move(p0), std::move(p1)};
}

Session make_session(
        asio::io_context& ctx,
        Response rs,
        asio::yield_context y)
{
    auto pipe = make_pipe(ctx);

    asio::spawn(ctx, [rs, &ctx, sink = move(pipe.sink)] (auto yield) mutable {
        http::async_write(sink, rs, yield);
    });

    Cancel c;
    return Session::create(move(pipe.source), false, c, y);
}

Session make_session(
        asio::io_context& ctx,
        Response rs,
        OuinetYield y)
{
    return make_session(ctx, move(rs), static_cast<asio::yield_context>(y));
}

Entry make_entry(
        asio::io_context& ctx,
        posix_time::ptime created,
        Response rs,
        asio::yield_context y)
{
    Session s = make_session(ctx, move(rs), y);
    return Entry{ created , move(s) };
}

Entry make_entry(
        asio::io_context& ctx,
        posix_time::ptime created,
        Response rs,
        OuinetYield y)
{
    return make_entry(ctx, move(created), move(rs), static_cast<asio::yield_context>(y));
}

BOOST_AUTO_TEST_CASE(test_cache_origin_fail)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    cc.parallel_fresh = [] (auto, auto) { return true; };

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto& c, auto y) {
        cache_check++;
        return make_entry(
                ctx,
                current_time(),
                {http::status::ok, rq.version()},
                y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;
        BOOST_CHECK(!ce);
        return or_throw<Session>(y, asio::error::connection_reset);
    };

    run_spawned(ctx, [&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            Cancel cancel;
            sys::error_code fresh_ec, cache_ec;
            auto s = cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
            BOOST_REQUIRE(fresh_ec);
            BOOST_REQUIRE(!cache_ec);
            auto& hdr = s.response_header();
            BOOST_CHECK_EQUAL(hdr.result(), http::status::ok);
        });

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_max_cached_age)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set( http::field::cache_control
              , str("max-age=", (cc.max_cached_age().total_seconds() + 10)));

        auto created = current_time() - cc.max_cached_age();

        if (rq.target() == "old") created -= seconds(5);
        else                      created += seconds(5);

        return make_entry(ctx, created, rs, y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;
        BOOST_CHECK(ce);
        BOOST_CHECK_EQUAL(rq.target(), "old");
        return make_session(ctx, {http::status::ok, rq.version()}, y);
    };

    run_spawned(ctx, [&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
                BOOST_REQUIRE(!fresh_ec);
                BOOST_REQUIRE(cache_ec);
            }
            {
                Request req{http::verb::get, "new", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);

                BOOST_REQUIRE(fresh_ec);
                BOOST_REQUIRE(!cache_ec);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_maxage)
{
    asio::io_context ctx;

    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
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

        return make_entry(ctx, created, rs, y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;
        BOOST_CHECK(ce);
        Response rs{http::status::ok, rq.version()};
        return make_session(ctx, rs, y);
    };

    run_spawned(ctx, [&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
            }
            {
                Request req{http::verb::get, "new", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_http10_expires)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

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

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
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

        return make_entry(ctx, created, rs, y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;
        BOOST_CHECK(ce);
        Response rs{http::status::ok, rq.version()};
        return make_session(ctx, rs, y);
    };

    run_spawned(ctx, [&](auto yield) {
            {
                Request req{http::verb::get, "old", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
            }
            {
                Request req{http::verb::get, "new", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_dont_load_cache_when_If_None_Match)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    cc.parallel_fresh = [] (auto, auto) { return true; };

    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
        BOOST_ERROR("Shouldn't go to cache");
        return make_entry(ctx, current_time(), Response{}, y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;
        BOOST_CHECK(!ce);
        Response rs{http::status::ok, rq.version()};
        rs.set("X-Test", "from-origin");
        return make_session(ctx, rs, y);
    };

    run_spawned(ctx, [&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            req.set(http::field::if_none_match, "abc");
            Cancel cancel;
            sys::error_code fresh_ec, cache_ec;
            auto s = cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
            auto& hdr = s.response_header();
            BOOST_CHECK_EQUAL(hdr.result(), http::status::ok);
        });

    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_no_etag_override)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    cc.parallel_fresh = [] (auto, auto) { return true; };

    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
        BOOST_ERROR("Shouldn't go to cache");
        return make_entry(ctx, current_time(), {}, y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;
        BOOST_CHECK(!ce);

        auto etag = get(rq, http::field::if_none_match);
        BOOST_CHECK(etag);
        BOOST_CHECK_EQUAL(*etag, "origin-etag");

        return make_session(ctx, {http::status::ok, rq.version()}, y);
    };

    run_spawned(ctx, [&](auto yield) {
            // In this test, the user agent provides its own etag.
            Request rq{http::verb::get, "mypage", 11};
            rq.set(http::field::if_none_match, "origin-etag");
            Cancel cancel;
            sys::error_code fresh_ec, cache_ec;
            cc.fetch(rq, dht_group, fresh_ec, cache_ec, cancel, yield);
        });

    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_request_no_store)
{
    Request rq{http::verb::get, "mypage", 11};
    rq.set(http::field::cache_control, "no-store");

    Response rs{http::status::ok, rq.version()};

    BOOST_REQUIRE(!CacheControl::ok_to_cache(rq, rs));
}

BOOST_AUTO_TEST_CASE(test_response_private)
{
    Request rq{http::verb::get, "mypage?foo=bar", 11};

    Response rs{http::status::ok, rq.version()};
    rs.set(http::field::cache_control, "private");

    BOOST_REQUIRE(!CacheControl::ok_to_cache(rq, rs));  // not private
    BOOST_REQUIRE(CacheControl::ok_to_cache(rq, rs, true));  // private
}

BOOST_AUTO_TEST_CASE(test_if_none_match)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
        cache_check++;

        Response rs{http::status::ok, rq.version()};
        rs.set(http::field::cache_control, "max-age=10");
        rs.set(http::field::etag, "123");
        rs.set("X-Test", "from-cache");

        return make_entry(ctx, current_time() - seconds(20), rs, y);
    };

    cc.fetch_fresh = [&](auto rq, auto ce, auto&, auto y) {
        origin_check++;

        auto etag = get(rq, http::field::if_none_match);
        BOOST_REQUIRE(etag);

        if (*etag == "123") {
            // No check for available cache entry since this may or may not be a revalidation.
            Response rs{http::status::not_modified, rq.version()};
            rs.set("X-Test", "from-origin-not-modified");
            return make_session(ctx, rs, y);
        }
        BOOST_CHECK(!ce);

        Response rs{http::status::ok, rq.version()};
        rs.set("X-Test", "from-origin-ok");

        return make_session(ctx, rs, y);
    };

    run_spawned(ctx, [&](auto yield) {
            {
                Request rq{http::verb::get, "mypage", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                auto s = cc.fetch(rq, dht_group, fresh_ec, cache_ec, cancel, yield);
                auto h = s.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-cache");
            }

            {

                // In this test, the user agent provides the existing etag.
                Request rq{http::verb::get, "mypage", 11};
                rq.set(http::field::if_none_match, "123");
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                auto s = cc.fetch(rq, dht_group, fresh_ec, cache_ec, cancel, yield);
                auto h = s.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::not_modified);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-origin-not-modified");
            }

            {
                // In this test, the user agent provides its own etag.
                Request rq{http::verb::get, "mypage", 11};
                rq.set(http::field::if_none_match, "abc");
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                auto s = cc.fetch(rq, dht_group, fresh_ec, cache_ec, cancel, yield);
                auto h = s.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-origin-ok");
            }
        });

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 3u);
}

BOOST_AUTO_TEST_CASE(test_req_no_cache_fresh_origin_ok)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto&, auto&, auto y) {
        cache_check++;
        Response rs{http::status::ok, rq.version()};
        // Return a fresh cached version.
        rs.set(http::field::cache_control, "max-age=3600");
        rs.set("X-Test", "from-cache");
        return make_entry(ctx, current_time(), rs, y);
    };

    cc.fetch_fresh = [&](auto rq, auto, auto&, auto y) {
        origin_check++;
        // No check for available cache entry since it may or may not have been checked.

        auto nocache = get(rq, http::field::cache_control);
        BOOST_REQUIRE(nocache);

        // Force using version from origin instead of validated version from cache
        // (i.e. not returning "304 Not Modified" here).
        Response rs{http::status::ok, rq.version()};
        rs.set("X-Test", "from-origin");
        return make_session(ctx, rs, y);
    };

    run_spawned(ctx, [&](auto yield) {
            {
                // Cached resources requested without "no-cache" should come from the cache
                // since the cached version is fresh enough.
                Request req{http::verb::get, "foo", 11};
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                auto s = cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
                auto h = s.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-cache");
            }
            {
                // Cached resources requested without "no-cache" should come from or be validated by the origin.
                // In this test we know it will be the origin.
                Request req{http::verb::get, "foo", 11};
                req.set(http::field::cache_control, "no-cache");
                Cancel cancel;
                sys::error_code fresh_ec, cache_ec;
                auto s = cc.fetch(req, dht_group, fresh_ec, cache_ec, cancel, yield);
                auto h = s.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-origin");
            }
        });

    // Cache should have been checked without "no-cache",
    // it may or may not have been checked with "no-cache".
    BOOST_CHECK(1u <= cache_check && cache_check < 3u);
    // Origin should have only been checked with "no-cache".
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_SUITE_END()
