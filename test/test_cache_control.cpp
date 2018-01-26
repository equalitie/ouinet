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

static posix_time::ptime now() {
    return posix_time::second_clock::universal_time();
}

BOOST_AUTO_TEST_CASE(test1)
{
    CacheControl cc;

    bool checked_cache = false;

    cc.fetch_from_cache = [&](auto rq, auto y) {
        Response rs{http::status::ok, rq.version()};
        checked_cache = true;
        return or_throw(y, sys::error_code(), Entry{now(), rs});
    };

    cc.fetch_from_origin = [&](auto rq, auto y) {
        BOOST_CHECK(false);
        return or_throw<Response>(y, sys::error_code());
    };

    asio::io_service ios;

    asio::spawn(ios, [&](auto yield) {
            Request req{http::verb::get, "foo", 11};
            sys::error_code ec;
            cc.fetch(req, yield[ec]);
            BOOST_CHECK(!ec);
        });

    ios.run();

    BOOST_CHECK(checked_cache);
}

BOOST_AUTO_TEST_SUITE_END()
