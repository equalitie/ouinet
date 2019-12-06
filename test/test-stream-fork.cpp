#define BOOST_TEST_MODULE stream_fork
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include "../src/stream/fork.h"
#include "../src/or_throw.h"
#include "../src/util/wait_condition.h"

BOOST_AUTO_TEST_SUITE(ouinet_stream_fork)

using namespace std;
using namespace ouinet;

using tcp = asio::ip::tcp;

pair<tcp::socket, tcp::socket>
make_connection(asio::io_context& ctx, asio::yield_context yield)
{
    using Ret = pair<tcp::socket, tcp::socket>;

    tcp::acceptor a(ctx, tcp::endpoint(tcp::v4(), 0));
    tcp::socket s1(ctx), s2(ctx);

    sys::error_code accept_ec;
    sys::error_code connect_ec;

    WaitCondition wc(ctx);

    asio::spawn(ctx, [&, lock = wc.lock()] (asio::yield_context yield) mutable {
            a.async_accept(s2, yield[accept_ec]);
        });

    s1.async_connect(a.local_endpoint(), yield[connect_ec]);
    wc.wait(yield);

    if (accept_ec)  return or_throw(yield, accept_ec, Ret(move(s1),move(s2)));
    if (connect_ec) return or_throw(yield, connect_ec, Ret(move(s1),move(s2)));

    return make_pair(move(s1), move(s2));
}

BOOST_AUTO_TEST_CASE(test_single) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source));

        stream::Fork<tcp::socket>::Tine tine(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        string rx_buf(tx_buf.size(), '\0');
        asio::async_read(tine, asio::buffer(rx_buf), yield[ec]);

        BOOST_REQUIRE_EQUAL(rx_buf, tx_buf);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_two) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source));

        stream::Fork<tcp::socket>::Tine tine1(fork);
        stream::Fork<tcp::socket>::Tine tine2(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        string rx_buf1(tx_buf.size(), '\0');
        asio::async_read(tine1, asio::buffer(rx_buf1), yield[ec]);
        BOOST_REQUIRE_EQUAL(rx_buf1, tx_buf);

        string rx_buf2(tx_buf.size(), '\0');
        asio::async_read(tine2, asio::buffer(rx_buf2), yield[ec]);
        BOOST_REQUIRE_EQUAL(rx_buf2, tx_buf);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_small_buffer_single) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source), 1);

        stream::Fork<tcp::socket>::Tine tine(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        string rx_buf(tx_buf.size(), '\0');
        asio::async_read(tine, asio::buffer(rx_buf), yield[ec]);

        BOOST_REQUIRE_EQUAL(rx_buf, tx_buf);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_small_buffer_two) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source), 1);

        stream::Fork<tcp::socket>::Tine tine1(fork);
        stream::Fork<tcp::socket>::Tine tine2(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        WaitCondition wc(ctx);
        
        asio::spawn(ctx,
            [&, lock = wc.lock()] (asio::yield_context yield)
            {
                string rx_buf1(tx_buf.size(), '\0');
                asio::async_read(tine1, asio::buffer(rx_buf1), yield[ec]);
                BOOST_REQUIRE_EQUAL(rx_buf1, tx_buf);
            });

        string rx_buf2(tx_buf.size(), '\0');
        asio::async_read(tine2, asio::buffer(rx_buf2), yield[ec]);
        BOOST_REQUIRE_EQUAL(rx_buf2, tx_buf);

        wc.wait(yield);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_two_small_buffers) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source));

        stream::Fork<tcp::socket>::Tine tine1(fork);
        stream::Fork<tcp::socket>::Tine tine2(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        WaitCondition wc(ctx);
        
        asio::spawn(ctx,
            [&, lock = wc.lock()] (asio::yield_context yield)
            {
                for (unsigned i = 0; i < tx_buf.size(); ++i) {
                    string rx_buf(1, '\0');
                    asio::async_read(tine1, asio::buffer(rx_buf), yield[ec]);
                    BOOST_REQUIRE(!ec);
                    BOOST_REQUIRE_EQUAL(rx_buf[0], tx_buf[i]);
                }
            });

        for (unsigned i = 0; i < tx_buf.size(); ++i) {
            string rx_buf(1, '\0');
            asio::async_read(tine2, asio::buffer(rx_buf), yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE_EQUAL(rx_buf[0], tx_buf[i]);
        }

        wc.wait(yield);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_close_fork) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        WaitCondition wc(ctx);

        {
            stream::Fork<tcp::socket> fork(move(source), 1);

            asio::spawn(ctx,
                [&, lock = wc.lock()] (asio::yield_context yield)
                {
                    sys::error_code ec;
                    stream::Fork<tcp::socket>::Tine tine(fork);
                    string rx_buf(1, '\0');
                    asio::async_read(tine, asio::buffer(rx_buf), yield[ec]);
                    BOOST_REQUIRE_EQUAL(ec, asio::error::operation_aborted);
                });

            fork.close();
        }

        wc.wait(yield);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_close_fork_after_read) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        WaitCondition wc(ctx);

        stream::Fork<tcp::socket> fork(move(source));
        stream::Fork<tcp::socket>::Tine tine(fork);

        asio::async_write(sink, asio::buffer("hello"), yield[ec]);

        string rx_buf(1, '\0');
        asio::async_read(tine, asio::buffer(rx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE_EQUAL(rx_buf, "h");

        asio::spawn(ctx,
            [&, lock = wc.lock()] (asio::yield_context yield)
            {
                sys::error_code ec;
                string rx_buf(1, '\0');
                asio::async_read(tine, asio::buffer(rx_buf), yield[ec]);
                BOOST_REQUIRE_EQUAL(ec, asio::error::operation_aborted);
            });

        fork.close();

        wc.wait(yield);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_close_one_tine) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source), 1);

        stream::Fork<tcp::socket>::Tine tine1(fork);
        stream::Fork<tcp::socket>::Tine tine2(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        WaitCondition wc(ctx);
        
        asio::spawn(ctx,
            [&, lock = wc.lock()] (asio::yield_context yield) mutable
            {
                string rx_buf(strlen("hello"), '\0');
                asio::async_read(tine1, asio::buffer(rx_buf), yield[ec]);
                BOOST_REQUIRE(!ec);
                BOOST_REQUIRE_EQUAL(rx_buf, "hello");
                auto l = make_shared<decltype(lock)>(wc.lock());
                ctx.post([&, l] () mutable { tine1.close(); });
            });

        for (unsigned i = 0; i < tx_buf.size();) {
            string rx_buf(std::min<size_t>(2, tx_buf.size() - i), '\0');
            size_t s = asio::async_read(tine2, asio::buffer(rx_buf), yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE_EQUAL(rx_buf.substr(0, s), tx_buf.substr(i, s));
            i += s;
        }

        wc.wait(yield);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_CASE(test_close_one_tine_while_blocked) {
    asio::io_context ctx;

    bool done = false;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        sys::error_code ec;
        tcp::socket source(ctx), sink(ctx);

        tie(source, sink) = make_connection(ctx, yield[ec]);
        BOOST_REQUIRE(!ec);

        stream::Fork<tcp::socket> fork(move(source), 1);

        stream::Fork<tcp::socket>::Tine tine1(fork);
        stream::Fork<tcp::socket>::Tine tine2(fork);

        string tx_buf = "hello world";
        asio::async_write(sink, asio::buffer(tx_buf), yield[ec]);
        BOOST_REQUIRE(!ec);

        WaitCondition wc(ctx);

        for (unsigned i = 0; i < tx_buf.size(); ++i) {
            string rx_buf(1, '\0');
            if (i == 1) {
                // Given that tine1 is not reading, the second call to
                // async_read on tine2 should block. Let's see whether
                // closing tine1 after that shall "release" reading on
                // tine2.
                auto l = make_shared<WaitCondition::Lock>(wc.lock());
                ctx.post([&, l] { tine1.close(); });
            }
            size_t s = asio::async_read(tine2, asio::buffer(rx_buf), yield[ec]);
            BOOST_REQUIRE(!ec);
            BOOST_REQUIRE_EQUAL(rx_buf.substr(0, s), tx_buf.substr(i, s));
        }

        wc.wait(yield);

        done = true;
    });

    ctx.run();

    BOOST_REQUIRE(done);
}

BOOST_AUTO_TEST_SUITE_END()


