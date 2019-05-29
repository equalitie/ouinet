#define BOOST_TEST_MODULE connection-pool
#include <boost/test/included/unit_test.hpp>

#include "connection_pool.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <chrono>

using namespace ouinet;

BOOST_AUTO_TEST_SUITE(connection_pool)

BOOST_AUTO_TEST_CASE(test_behavior)
{
    asio::io_service ios;

    asio::spawn(ios, [&ios] (asio::yield_context yield) {
        asio::ip::tcp::acceptor server(ios, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 50123));
        server.listen();

        asio::ip::tcp::socket connection(ios);
        server.async_accept(connection, yield);
        server.close();

        asio::async_write(connection, asio::const_buffer("test1\n", 6), yield);

        {
            asio::steady_timer timer(ios);
            timer.expires_from_now(std::chrono::milliseconds(1000));
            timer.async_wait(yield);
        }

        asio::async_write(connection, asio::const_buffer("test2\n", 6), yield);

        {
            asio::steady_timer timer(ios);
            timer.expires_from_now(std::chrono::milliseconds(1000));
            timer.async_wait(yield);
        }

        asio::async_write(connection, asio::const_buffer("test3\n", 6), yield);

        {
            asio::steady_timer timer(ios);
            timer.expires_from_now(std::chrono::milliseconds(2000));
            timer.async_wait(yield);
        }

        asio::async_write(connection, asio::const_buffer("test4\n", 6), yield);

        {
            asio::steady_timer timer(ios);
            timer.expires_from_now(std::chrono::milliseconds(1000));
            timer.async_wait(yield);
        }

        asio::async_write(connection, asio::const_buffer("test5\n", 6), yield);

        connection.close();
    });

    asio::spawn(ios, [&ios] (asio::yield_context yield) {
        ConnectionPool<std::string> pool;

        {
            asio::ip::tcp::socket connection(ios);
            connection.async_connect(asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), 50123), yield);

            char buffer[6];
            asio::async_read(connection, asio::mutable_buffer(buffer, 6), yield);
            BOOST_CHECK_EQUAL(std::string(buffer, 6), "test1\n");

            GenericStream stream{std::move(connection)};
            ConnectionPool<std::string>::Connection pooled_connection = pool.wrap(std::move(stream));
            *pooled_connection = "test";

            pool.push_back(std::move(pooled_connection));
        }

        {
            BOOST_CHECK(!pool.empty());
            ConnectionPool<std::string>::Connection connection = pool.pop_front();
            BOOST_CHECK(pool.empty());
            BOOST_CHECK_EQUAL(*connection, "test");

            /*
             * Server sleeps for 1 second, so the async_read call
             * is invoked before the idle read returns.
             */

            char buffer[6];
            asio::async_read(connection, asio::mutable_buffer(buffer, 6), yield);
            BOOST_CHECK_EQUAL(std::string(buffer, 6), "test2\n");

            pool.push_back(std::move(connection));
        }

        {
            BOOST_CHECK(!pool.empty());
            ConnectionPool<std::string>::Connection connection = pool.pop_front();
            BOOST_CHECK(pool.empty());
            BOOST_CHECK_EQUAL(*connection, "test");

            /*
             * Server sleeps for 1 second and client sleeps for 2 seconds,
             * so the idle read returns before the async_read call is invoked.
             */

            {
                asio::steady_timer timer(ios);
                timer.expires_from_now(std::chrono::milliseconds(2000));
                timer.async_wait(yield);
            }

            {
                char buffer[6];
                asio::async_read(connection, asio::mutable_buffer(buffer, 6), yield);
                BOOST_CHECK_EQUAL(std::string(buffer, 6), "test3\n");
            }

            /*
             * Two calls with no intervening reinsertion into the pool should
             * result in an async_read_some invocation directly on the tcp::socket.
             */

            {
                char buffer[6];
                asio::async_read(connection, asio::mutable_buffer(buffer, 6), yield);
                BOOST_CHECK_EQUAL(std::string(buffer, 6), "test4\n");
            }

            pool.push_back(std::move(connection));
        }

        {
            {
                asio::steady_timer timer(ios);
                timer.expires_from_now(std::chrono::milliseconds(2000));
                timer.async_wait(yield);
            }

            /*
             * Server sleeps for 1 second and client sleeps for 2 seconds,
             * so data is received while the connection is idle and the connection
             * is closed by the Pool.
             */

            BOOST_CHECK(pool.empty());
        }

    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
