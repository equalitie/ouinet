#define BOOST_TEST_MODULE blocker
#include <boost/test/included/unit_test.hpp>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>

#include <http_util.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_util)

namespace http = boost::beast::http;

using Request = http::request<http::empty_body>;

BOOST_AUTO_TEST_CASE(test_filter_fields) {
    Request orig_rq;
    orig_rq.method(http::verb::get);
    orig_rq.target("http://example.com/");
    orig_rq.version(11);
    orig_rq.set("X-Foo-Bar", "foo");
    orig_rq.set(http::field::referer, "example.net");
    orig_rq.set(http::field::host, "example.com");
    orig_rq.set("X-OuInEt-Foo", "bar");

    Request filt_rq = ouinet::util::filter_fields(orig_rq, http::field::host);

    // These should not be changed.
    BOOST_REQUIRE(filt_rq.method() == orig_rq.method());
    BOOST_REQUIRE(filt_rq.target() == orig_rq.target());
    BOOST_REQUIRE(filt_rq.version() == orig_rq.version());
    // This should be explicitly kept.
    BOOST_REQUIRE(filt_rq[http::field::host] == orig_rq[http::field::host]);
    // This should be implicitly kept.
    BOOST_REQUIRE(filt_rq["X-Ouinet-Foo"] == orig_rq["X-Ouinet-Foo"]);
    // This should be filtered out.
    BOOST_REQUIRE(filt_rq["X-Foo-Bar"] == "");
    BOOST_REQUIRE(filt_rq[http::field::referer] == "");
}

BOOST_AUTO_TEST_SUITE_END()

