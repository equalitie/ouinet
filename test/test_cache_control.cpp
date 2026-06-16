#define BOOST_TEST_MODULE cache_control
#include <boost/test/unit_test.hpp>
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

#include "util/async_test.h"
#include "util/unwrap.h"

BOOST_AUTO_TEST_SUITE(ouinet_cache_control)

using namespace std;
using namespace ouinet;
namespace error = asio::error;
namespace posix_time = boost::posix_time;
using Entry    = CacheEntry;
using Request  = http::request<http::string_body>;
using Response = CacheControl::Response;
using posix_time::seconds;
using boost::optional;
using beast::string_view;
using ouinet::util::str;

static const string dht_group("fake-dht-group");

static posix_time::ptime current_time() {
    return posix_time::second_clock::universal_time();
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
    explicit readable_pipe_patched(util::AsioExecutor exec) : asio::readable_pipe{exec} {}

    template <typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(const ConstBufferSequence& buffer, WriteHandler handler) { assert(false); }
};

struct Pipe {
    readable_pipe_patched source;
    asio::writable_pipe sink;
};

Pipe make_pipe(util::AsioExecutor exec) {
    readable_pipe_patched p0{exec};
    asio::writable_pipe p1{exec};
    asio::connect_pipe(p0, p1);
    return {std::move(p0), std::move(p1)};
}

Session make_session(Response rs, Async yield) {
    auto pipe = make_pipe(yield.get_executor());

    task::spawn_detached(yield.get_executor(), [rs, sink = move(pipe.sink)] (auto yield) mutable {
        http::async_write(sink, rs, yield);
    });

    return unwrap(Session::create(move(pipe.source), false, yield));
}

Entry make_entry(posix_time::ptime created, Response rs, Async yield) {
    Session s = make_session(move(rs), yield);
    return Entry{ created , move(s) };
}

BOOST_AUTO_TEST_CASE(test_cache_origin_fail)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto yield) {
        cache_check++;

        Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
        rs.set("X-Test", "from-cache");

        return make_entry(current_time(), rs, yield);
    };

    cc.fetch_fresh = [&](auto rq, auto yield) {
        origin_check++;
        return std::unexpected(asio::error::connection_reset);
    };

    async_test(ctx, [&](auto yield) {
        Request normal_request{http::verb::get, "http://foo", 11};
        normal_request.set(http_::request_group_hdr, dht_group);

        auto req = CacheRequest::from(normal_request).value();
        auto s = cc.fetch(req, yield).value();
        auto& hdr = s.response_header();
        BOOST_REQUIRE_EQUAL(hdr.result(), http::status::ok);
        BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-cache");
    });
    ctx.run();

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 1u);
}

void yield_now(Async yield) {
    auto exec = yield.get_executor();

    return boost::asio::async_initiate<Async, void()>(
        [exec = std::move(exec)](auto handler) {
            boost::asio::post(exec, std::move(handler));
        },
        yield
    );
}

BOOST_AUTO_TEST_CASE(test_max_cached_age)
{
    asio::io_context ctx;

    CacheControl cc(ctx, "test");

    // This test is set up such that `fetch_stored` always completes before `fetch_fresh`. That
    // means that when the new resource is requested, it's retrieved from the cache, but if the old
    // resource is requested, it's first retrieved from the cache, discarded (for being too old) and
    // then retrieved fresh.

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    async_test(ctx, [&](auto yield) {
        auto old_resource_id = cache::ResourceId::from_url("http://old");
        auto new_resource_id = cache::ResourceId::from_url("http://new");

        cc.fetch_stored = [&](auto rq, auto yield) {
            cache_check++;

            Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-cache");
            rs.set( http::field::cache_control
                  , str("max-age=", (cc.max_cached_age().total_seconds() + 10)));

            auto created = current_time() - cc.max_cached_age();

            if (rq.resource_id() == old_resource_id) created -= seconds(5);
            else                                     created += seconds(5);

            return make_entry(created, rs, yield);
        };

        cc.fetch_fresh = [&](auto rq, auto yield) {
            origin_check++;

            switch (origin_check) {
                case 1: BOOST_CHECK_EQUAL(rq.resource_id(), old_resource_id); break;
                case 2: BOOST_CHECK_EQUAL(rq.resource_id(), new_resource_id); break;
                default: BOOST_FAIL("fetch_fresh should be called exactly two times");
            }

            Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-origin");

            auto session = make_session(rs, yield);

            // Insert short delay to ensure `fetch_stored` completes first
            async_sleep(10ms, yield);

            return session;
        };

        {
            Request normal_req{http::verb::get, "http://old", 11};
            normal_req.set(http_::request_group_hdr, dht_group);

            auto req = CacheRequest::from(normal_req).value();
            auto session = cc.fetch(req, yield).value();
            auto hdr = session.response_header();
            BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-origin");
        }
        {
            Request normal_req{http::verb::get, "http://new", 11};
            normal_req.set(http_::request_group_hdr, dht_group);

            auto req = CacheRequest::from(normal_req).value();
            auto session = cc.fetch(req, yield).value();
            auto hdr = session.response_header();
            BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-cache");
        }
    });
    ctx.run();

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 2u);
}

BOOST_AUTO_TEST_CASE(test_maxage)
{
    asio::io_context ctx;

    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    async_test(ctx, [&](auto yield) {
        auto old_resource_id = cache::ResourceId::from_url("http://old");
        auto new_resource_id = cache::ResourceId::from_url("http://new");

        cc.fetch_stored = [&](auto rq, auto yield) {
            cache_check++;

            Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-cache");
            rs.set(http::field::cache_control, "max-age=60");

            auto created = current_time();

            if (rq.resource_id() == old_resource_id) {
                created -= seconds(120);
            }
            else {
                created -= seconds(30);
                BOOST_CHECK(rq.resource_id() == new_resource_id);
            }

            return make_entry(created, rs, yield);
        };

        cc.fetch_fresh = [&](auto rq, auto yield) {
            origin_check++;

            Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-origin");
            auto session = make_session(rs, yield);

            // Insert short delay to ensure `fetch_stored` completes first
            async_sleep(10ms, yield);

            return session;
        };

        {
            Request normal_req{http::verb::get, "http://old", 11};
            normal_req.set(http_::request_group_hdr, dht_group);

            auto req = CacheRequest::from(normal_req).value();
            auto session = cc.fetch(req, yield).value();
            auto hdr = session.response_header();
            BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-origin");
        }
        {
            Request normal_req{http::verb::get, "http://new", 11};
            normal_req.set(http_::request_group_hdr, dht_group);

            auto req = CacheRequest::from(normal_req).value();
            auto session = cc.fetch(req, yield).value();
            auto hdr = session.response_header();
            BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-cache");
        }
    });
    ctx.run();

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 2u);
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

    async_test(ctx, [&](auto yield) {
        auto old_resource_id = cache::ResourceId::from_url("http://old");
        auto new_resource_id = cache::ResourceId::from_url("http://new");

        cc.fetch_stored = [&](auto rq, auto yield) {
            cache_check++;

            Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-cache");

            auto created = current_time();

            if (rq.resource_id() == old_resource_id) {
                rs.set( http::field::expires
                      , format_time(current_time() - posix_time::seconds(10)));
            }
            else {
                BOOST_CHECK(rq.resource_id() == new_resource_id);
                rs.set( http::field::expires
                      , format_time(current_time() + posix_time::seconds(10)));
            }

            return make_entry(created, rs, yield);
        };

        cc.fetch_fresh = [&](auto rq, auto yield) {
            origin_check++;

            Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-origin");
            auto session = make_session(rs, yield);

            // Insert short delay to ensure `fetch_stored` completes first
            async_sleep(10ms, yield);

            return session;
        };

        {
            Request normal_req{http::verb::get, "http://old", 11};
            normal_req.set(http_::request_group_hdr, dht_group);

            auto req = CacheRequest::from(normal_req).value();
            auto session = cc.fetch(req, yield).value();
            auto hdr = session.response_header();
            BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-origin");
        }
        {
            Request normal_req{http::verb::get, "http://new", 11};
            normal_req.set(http_::request_group_hdr, dht_group);

            auto req = CacheRequest::from(normal_req).value();
            auto session = cc.fetch(req, yield).value();
            auto hdr = session.response_header();
            BOOST_REQUIRE_EQUAL(hdr["X-Test"], "from-cache");
        }
    });
    ctx.run();

    BOOST_CHECK_EQUAL(cache_check, 2u);
    BOOST_CHECK_EQUAL(origin_check, 2u);
}

BOOST_AUTO_TEST_CASE(test_dont_load_cache_when_If_None_Match)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        BOOST_ERROR("Shouldn't go to cache");
        return make_entry(current_time(), Response{}, y);
    };

    cc.fetch_fresh = [&](auto rq, auto yield) {
        origin_check++;
        Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
        rs.set("X-Test", "from-origin");
        return make_session(rs, yield);
    };

    async_test(ctx, [&](auto yield) {
        Request normal_req{http::verb::get, "http://foo", 11};
        normal_req.set(http::field::if_none_match, "abc");
        normal_req.set(http_::request_group_hdr, dht_group);
        auto req = CacheRequest::from(normal_req).value();
        auto session = cc.fetch(req, yield).value();
        auto& hdr = session.response_header();
        BOOST_CHECK_EQUAL(hdr.result(), http::status::ok);
        BOOST_CHECK_EQUAL(hdr["X-Test"], "from-origin");
    });
    ctx.run();

    BOOST_CHECK_EQUAL(origin_check, 1u);
}

BOOST_AUTO_TEST_CASE(test_no_etag_override)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto y) {
        BOOST_ERROR("Shouldn't go to cache");
        return make_entry(current_time(), {}, y);
    };

    cc.fetch_fresh = [&](auto rq, auto yield) {
        origin_check++;

        auto etag = rq.get_if_none_match_field();
        BOOST_CHECK(etag);
        BOOST_CHECK_EQUAL(*etag, "origin-etag");

        return make_session({http::status::ok, CacheRequest::HTTP_VERSION}, yield);
    };

    async_test(ctx, [&](auto yield) {
        // In this test, the user agent provides its own etag.
        Request normal_rq{http::verb::get, "http://mypage", 11};
        normal_rq.set(http::field::if_none_match, "origin-etag");
        normal_rq.set(http_::request_group_hdr, dht_group);

        auto rq = CacheRequest::from(normal_rq).value();
        cc.fetch(rq, yield).value();
    });
    ctx.run();

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

    cc.fetch_stored = [&](auto rq, auto yield) {
        cache_check++;

        Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
        rs.set(http::field::cache_control, "max-age=10");
        rs.set(http::field::etag, "123");
        rs.set("X-Test", "from-cache");

        return make_entry(current_time() - seconds(20), rs, yield);
    };

    cc.fetch_fresh = [&](auto rq, auto yield) {
        origin_check++;

        auto etag = rq.get_if_none_match_field();

        // In the first fresh request, `if_none_match` field is not present, but after the cached
        // response arrives, the fresh request is restarted with it. In the subsequent requests,
        // `if_none_match` is always present.
        if (origin_check == 1) {
            BOOST_REQUIRE(!etag);
        } else {
            BOOST_REQUIRE(etag);
        }

        // Insert short delay to ensure `fetch_stored` completes first
        async_sleep(10ms, yield);

        if (etag && *etag == "123") {
            // No check for available cache entry since this may or may not be a revalidation.
            Response rs{http::status::not_modified, CacheRequest::HTTP_VERSION};
            rs.set("X-Test", "from-origin-not-modified");
            return make_session(rs, yield);
        }

        Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
        rs.set("X-Test", "from-origin-ok");

        return make_session(rs, yield);
    };

    async_test(ctx, [&](auto yield) {
            {
                Request normal_rq{http::verb::get, "http://mypage", 11};
                normal_rq.set(http_::request_group_hdr, dht_group);
                auto rq = CacheRequest::from(normal_rq).value();

                auto session = cc.fetch(rq, yield).value();
                auto h = session.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-cache");
            }

            {
                // In this test, the user agent provides the existing etag.
                Request normal_rq{http::verb::get, "http://mypage", 11};
                normal_rq.set(http::field::if_none_match, "123");
                normal_rq.set(http_::request_group_hdr, dht_group);
                auto rq  = CacheRequest::from(normal_rq).value();

                auto session = cc.fetch(rq, yield).value();
                auto h = session.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::not_modified);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-origin-not-modified");
            }

            {
                // In this test, the user agent provides its own etag.
                Request normal_rq{http::verb::get, "http://mypage", 11};
                normal_rq.set(http::field::if_none_match, "abc");
                normal_rq.set(http_::request_group_hdr, dht_group);
                auto rq  = CacheRequest::from(normal_rq).value();

                auto session = cc.fetch(rq, yield).value();
                auto h = session.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-origin-ok");
            }
        });
    ctx.run();

    BOOST_CHECK_EQUAL(cache_check, 1u);
    BOOST_CHECK_EQUAL(origin_check, 4u);
}

BOOST_AUTO_TEST_CASE(test_req_no_cache_fresh_origin_ok)
{
    asio::io_context ctx;
    CacheControl cc(ctx, "test");

    unsigned cache_check = 0;
    unsigned origin_check = 0;

    cc.fetch_stored = [&](auto rq, auto yield) {
        cache_check++;
        Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
        // Return a fresh cached version.
        rs.set(http::field::cache_control, "max-age=3600");
        rs.set("X-Test", "from-cache");
        return make_entry(current_time(), rs, yield);
    };

    cc.fetch_fresh = [&](auto rq, auto yield) {
        origin_check++;
        // No check for available cache entry since it may or may not have been checked.

        auto nocache = rq.get_cache_control_field();

        if (origin_check == 1) {
            BOOST_REQUIRE(!nocache);
        } else {
            BOOST_REQUIRE(nocache);
        }

        // Insert short delay to ensure `fetch_stored` completes first
        async_sleep(10ms, yield);

        // Force using version from origin instead of validated version from cache
        // (i.e. not returning "304 Not Modified" here).
        Response rs{http::status::ok, CacheRequest::HTTP_VERSION};
        rs.set("X-Test", "from-origin");
        return make_session(rs, yield);
    };

    async_test(ctx, [&](auto yield) {
            {
                // Cached resources requested without "no-cache" should come from the cache
                // since the cached version is fresh enough.
                Request normal_req{http::verb::get, "http://foo", 11};
                normal_req.set(http_::request_group_hdr, dht_group);
                auto req = CacheRequest::from(normal_req).value();
                auto session = cc.fetch(req, yield).value();
                auto h = session.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-cache");
            }
            {
                // Cached resources requested without "no-cache" should come from or be validated by the origin.
                // In this test we know it will be the origin.
                Request normal_req{http::verb::get, "http://foo", 11};
                normal_req.set(http::field::cache_control, "no-cache");
                normal_req.set(http_::request_group_hdr, dht_group);

                auto req = CacheRequest::from(normal_req).value();
                auto session = cc.fetch(req, yield).value();
                auto h = session.response_header();
                BOOST_CHECK_EQUAL(h.result(), http::status::ok);
                BOOST_CHECK_EQUAL(h["X-Test"], "from-origin");
            }
        });
    ctx.run();

    // Cache should have been checked without "no-cache",
    // it may or may not have been checked with "no-cache".
    BOOST_CHECK(1u <= cache_check && cache_check < 3u);
    // Origin is invoked in both cases, but only used in the "no-cache" case.
    BOOST_CHECK_EQUAL(origin_check, 2u);
}

BOOST_AUTO_TEST_SUITE_END()
