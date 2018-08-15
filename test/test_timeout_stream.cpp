#define BOOST_TEST_MODULE timeout_stream
#include <boost/test/included/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <iostream>

#include <timeout_stream.h>

BOOST_AUTO_TEST_SUITE(ouinet_timeout_stream)

using namespace std;
using namespace ouinet;
using namespace chrono;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void async_sleep( asio::io_context& ioc
                , asio::steady_timer::duration duration
                , asio::yield_context yield)
{
    asio::steady_timer timer(ioc);
    timer.expires_from_now(duration);
    sys::error_code ec;
    timer.async_wait(yield[ec]);
}

Clock::duration time_delta(Clock::time_point t1, Clock::time_point t2) {
    if (t1 >= t2) return t1 - t2;
    return t2 - t1;
}

bool about_equal(Clock::time_point t1, Clock::time_point t2) {
    return time_delta(t1, t2) < 20ms;
}

Clock::time_point now() {
    return Clock::now();
}

unsigned ms(Clock::duration d) {
    return chrono::duration_cast<chrono::milliseconds>(d).count();
}

BOOST_AUTO_TEST_CASE(test_read_timeout_1) {
    asio::io_context ioc;

    tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 0));

    asio::spawn(ioc, [&](auto yield) {
        tcp::socket s(ioc);
        acceptor.async_accept(s, yield);

        auto timeout_duration = 500ms;

        TimeoutStream<tcp::socket> t(move(s));
        t.set_read_timeout(timeout_duration);

        std::string rx_buf(1, '\0');

        sys::error_code ec;
        auto start = now();
        asio::async_read(t, asio::buffer(rx_buf), yield[ec]);

        BOOST_REQUIRE(about_equal(start + timeout_duration, now()));
        BOOST_REQUIRE_EQUAL(ec, asio::error::timed_out);
    });

    spawn(ioc, [&](auto yield) {
        tcp::socket s(ioc);
        s.async_connect(acceptor.local_endpoint(), yield);
        async_sleep(ioc, 1s, yield);
    });

    ioc.run();
}

BOOST_AUTO_TEST_CASE(test_read_timeout_2) {
    asio::io_context ioc;

    tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 0));

    asio::spawn(ioc, [&](auto yield) {
        tcp::socket s(ioc);
        acceptor.async_accept(s, yield);

        auto timeout_duration = 500ms;

        TimeoutStream<tcp::socket> t(move(s));
        t.set_read_timeout(timeout_duration);

        std::string rx_buf(1, '\0');
        sys::error_code ec;

        {
            auto start = now();
            asio::async_read(t, asio::buffer(rx_buf), yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE(about_equal(start + 250ms, now()));
            BOOST_REQUIRE_EQUAL(rx_buf[0], 'a');
        }

        {
            auto start = now();
            asio::async_read(t, asio::buffer(rx_buf), yield[ec]);
            BOOST_REQUIRE(about_equal(start + timeout_duration, now()));
            BOOST_REQUIRE_EQUAL(ec, asio::error::timed_out);
        }
    });

    spawn(ioc, [&](auto yield) {
        tcp::socket s(ioc);
        s.async_connect(acceptor.local_endpoint(), yield);

        async_sleep(ioc, 250ms, yield);

        sys::error_code ec;
        string tx_buf("a");
        asio::async_write(s, asio::buffer(tx_buf), yield[ec]);

        async_sleep(ioc, 1s, yield);
    });

    ioc.run();
}

BOOST_AUTO_TEST_SUITE_END()

