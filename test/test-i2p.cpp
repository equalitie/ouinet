#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>
#include <boost/filesystem.hpp>

#include <boost/asio/spawn.hpp>
#include <iostream>
#include <chrono>

#include <vector>
#include <random>
#include <climits>
#include <algorithm>
#include <functional>

#include <namespaces.h>
#include <async_sleep.h>
#include <util/async_generator.h>
#include <util/wait_condition.h>
#include <ouiservice/i2p.h>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/variance.hpp>

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

using namespace std;
using namespace ouinet;
using namespace chrono;
using namespace chrono_literals;
namespace test = boost::unit_test;
using ouiservice::i2poui::Service;
using ouiservice::i2poui::Server;
using ouiservice::i2poui::Client;

const std::string hello_message = "hello";

struct Setup {
    string testname;
    string testsuite;
    fs::path tempdir;

    Setup()
        : testname(test::framework::current_test_case().p_name)
        , testsuite(test::framework::get<test::test_suite>(test::framework::current_test_case().p_parent_id).p_name)
        , tempdir(fs::temp_directory_path() / "ouinet-cpp-tests" / testsuite / testname / fs::unique_path())
    {
        fs::create_directories(tempdir);
    }

    ~Setup() {
        fs::remove_all(tempdir);
    }
};

template<class Rep, class Period>
float as_seconds(std::chrono::duration<Rep, Period> duration) {
    return duration_cast<milliseconds>(duration).count() / 1000.f;
}

BOOST_AUTO_TEST_CASE(test_connect_and_exchage) {
    Setup setup;

    asio::io_context ctx;

    struct SharedState {
        SharedState(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<Service>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        shared_ptr<Service> service;
        WaitCondition server_ready;
        WaitCondition client_finished;
        WaitCondition::Lock client_finished_lock;
        string server_ep;
    };

    auto shared = make_shared<SharedState>(setup, ctx.get_executor());

    // Server
    asio::spawn(ctx, [shared, server_ready_lock = shared->server_ready.lock()] (asio::yield_context yield) {
        auto server = shared->service->build_server("i2p-private-key");

        shared->server_ep = server->public_identity();

        sys::error_code ec;

        BOOST_TEST_MESSAGE("Server starts listening");
        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        BOOST_TEST_MESSAGE("Server accepting");
        GenericStream conn = server->accept_without_handshake(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept: " << ec.message());

        BOOST_TEST_MESSAGE("Server writing hello message");
        asio::async_write(conn, asio::buffer(hello_message), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server write: " << ec.message());

        shared->client_finished.wait(yield);
    });


    // Client
    asio::spawn(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Await server_ready (this may take a while)");
        shared->server_ready.wait(yield);
        BOOST_TEST_MESSAGE("Server is ready");

        auto client = shared->service->build_client(shared->server_ep);

        sys::error_code ec;

        BOOST_TEST_MESSAGE("Client starting");
        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        Cancel cancel;

        BOOST_TEST_MESSAGE("Client connecting");
        auto conn = client->connect_without_handshake(yield[ec], cancel);
        BOOST_TEST_REQUIRE(!ec, "Client connect: " << ec.message());

        BOOST_TEST_MESSAGE("Client reading hello message");
        std::string buffer(hello_message.size(), 'X');
        asio::async_read(conn, asio::buffer(buffer), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Client read: " << ec.message());

        BOOST_REQUIRE_EQUAL(buffer, hello_message);

        shared->client_finished_lock.release();
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_connect_with_retry_and_exchage) {
    Setup setup;

    asio::io_context ctx;

    struct SharedState {
        SharedState(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<Service>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        Cancel cancel;
        shared_ptr<Service> service;
        WaitCondition server_ready;
        WaitCondition client_finished;
        WaitCondition::Lock client_finished_lock;
        string server_ep;
    };

    BOOST_TEST_MESSAGE("Preparing shared state");
    auto shared = make_shared<SharedState>(setup, ctx.get_executor());

    // Server
    asio::spawn(ctx, [shared, server_ready_lock = shared->server_ready.lock()] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Server spawned");
        auto server = shared->service->build_server("i2p-private-key");

        shared->server_ep = server->public_identity();

        sys::error_code ec;

        auto cancelled = shared->cancel.connect([&] { server->stop_listen(); });

        BOOST_TEST_MESSAGE("Server starts listening");
        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        server->accept(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept with retry: " << ec.message());

        shared->client_finished.wait(yield);
    });


    // Client
    asio::spawn(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Client awaits server_ready (this may take a while)");
        shared->server_ready.wait(yield);
        BOOST_TEST_MESSAGE("Server is ready");

        auto client = shared->service->build_client(shared->server_ep);

        sys::error_code ec;

        BOOST_TEST_MESSAGE("Client starting");
        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        BOOST_TEST_MESSAGE("Client connecting");
        client->connect(yield[ec], shared->cancel);
        BOOST_TEST_REQUIRE(!ec, "Client connect with retries: " << ec.message());

        // Tell the server we're done.
        shared->client_finished_lock.release();
        shared->cancel();
    });

    ctx.run();
}

std::vector<unsigned char> generate_random_bytes(size_t size) {
    using random_bytes_engine = std::independent_bits_engine<
    std::default_random_engine, CHAR_BIT, unsigned char>;

    random_bytes_engine rbe;
    std::vector<unsigned char> data(size);
    std::generate(begin(data), end(data), std::ref(rbe));
    return data;
}

std::string byte_units(uint64_t count) {
    const uint64_t mb = 1024 * 1024;
    const uint64_t kb = 1024;

    if (count >= 1024 * 1024) {
        auto mbs = count / mb;
        auto rest = float((count - (mbs*mb))) / mb;
        return util::str(mbs, ".", int(rest*1000), "MiB"); 
    } else if (count >= kb) {
        auto kbs = count / kb;
        auto rest = float((count - (kbs*kb))) / kb;
        return util::str(kbs, ".", int(rest*1000), "KiB"); 
    } else {
        return util::str(count, "B"); 
    }
}

BOOST_AUTO_TEST_CASE(test_speed) {
    Setup setup;

    asio::io_context ctx;

    struct SharedState {
        SharedState(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<Service>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        Cancel cancel;
        shared_ptr<Service> service;
        WaitCondition server_ready;
        WaitCondition client_finished;
        WaitCondition::Lock client_finished_lock;
        string server_ep;
        unsigned int buffer_size = 512;
        unsigned int message_count = 5 * 1024 * 1024 / buffer_size;
        steady_clock::time_point send_started;
        std::queue<std::vector<unsigned char>> sent_messages;
    };

    BOOST_TEST_MESSAGE("Preparing shared state");
    auto shared = make_shared<SharedState>(setup, ctx.get_executor());

    // Server
    asio::spawn(ctx, [shared, server_ready_lock = shared->server_ready.lock()] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Server spawned");
        auto server = shared->service->build_server("i2p-private-key");

        shared->server_ep = server->public_identity();

        sys::error_code ec;

        auto cancelled = shared->cancel.connect([&] { server->stop_listen(); });

        BOOST_TEST_MESSAGE("Server starts listening");
        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        auto conn = server->accept(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept with retries: " << ec.message());

        std::vector<unsigned char> buffer(shared->buffer_size);

        for (uint32_t i = 0; i < shared->message_count; i++) {
            if (i % 512 == 0 && i != 0) {
                BOOST_TEST_MESSAGE("Server received " << i << " out of " << shared->message_count << " messages so far");
            }
            asio::async_read(conn, asio::buffer(buffer), yield[ec]);
            BOOST_TEST_REQUIRE(!ec, "Server read: " << ec.message());
            assert(!shared->sent_messages.empty());

            auto expected = std::move(shared->sent_messages.front());
            shared->sent_messages.pop();

            BOOST_TEST_REQUIRE(expected == buffer);
        }

        auto end = steady_clock::now();
        auto bytes = (shared->buffer_size * shared->message_count);
        auto elapsed_ms = duration_cast<milliseconds>(end - shared->send_started).count();
        float elapsed_s = elapsed_ms / 1000.f;

        std::cout << "Total received " << bytes << " Bytes in " << elapsed_ms << "ms\n";
        std::cout << "Which is about " << byte_units(bytes / elapsed_s) << "/s\n";


        shared->client_finished.wait(yield);
    });


    // Client
    asio::spawn(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Client awaits server_ready (this may take a while)");
        shared->server_ready.wait(yield);
        BOOST_TEST_MESSAGE("Server is ready");

        //RetryingClient client{shared->service};
        auto client = shared->service->build_client(shared->server_ep);

        sys::error_code ec;

        BOOST_TEST_MESSAGE("Client starting");
        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        BOOST_TEST_MESSAGE("Client connecting");
        auto conn = client->connect(yield[ec], shared->cancel);
        BOOST_TEST_REQUIRE(!ec, "Client connect with retries: " << ec.message());

        shared->send_started = steady_clock::now();

        for (uint32_t i = 0; i < shared->message_count; i++) {
            shared->sent_messages.push(generate_random_bytes(shared->buffer_size));
            asio::async_write(conn, asio::buffer(shared->sent_messages.back()), yield[ec]);
            BOOST_TEST_REQUIRE(!ec, "Client sending buffer #" << i << ": " << ec.message());
        }

        // Tell the server we're done.
        shared->client_finished_lock.release();
        shared->cancel();
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_subsequent_connection_speed) {
    Setup setup;

    asio::io_context ctx;

    struct SharedState {
        SharedState(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<Service>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        Cancel cancel;
        shared_ptr<Service> service;
        WaitCondition server_ready;
        WaitCondition client_finished;
        WaitCondition::Lock client_finished_lock;
        string server_ep;
        unsigned subsequent_conn_count = 32;
    };

    BOOST_TEST_MESSAGE("Preparing shared state");
    auto shared = make_shared<SharedState>(setup, ctx.get_executor());

    // Server
    asio::spawn(ctx, [shared, server_ready_lock = shared->server_ready.lock()] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Server spawned");
        auto server = shared->service->build_server("i2p-private-key");

        shared->server_ep = server->public_identity();

        sys::error_code ec;

        auto cancelled = shared->cancel.connect([&] { server->stop_listen(); });

        BOOST_TEST_MESSAGE("Server starts listening");
        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        auto conn0 = server->accept(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept #0 with retries: " << ec.message());

        for (unsigned i = 0; i < shared->subsequent_conn_count; i++) {
            auto conn = server->accept(yield[ec]);
            BOOST_TEST_REQUIRE(!ec, "Server accept #" << (i+1) << ": " << ec.message());
        }

        shared->client_finished.wait(yield);
    });


    // Client
    asio::spawn(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Client awaits server_ready (this may take a while)");
        shared->server_ready.wait(yield);
        BOOST_TEST_MESSAGE("Server is ready");

        auto client = shared->service->build_client(shared->server_ep);

        sys::error_code ec;

        BOOST_TEST_MESSAGE("Client starting");
        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        steady_clock::time_point conn0_start = steady_clock::now();

        BOOST_TEST_MESSAGE("Client connecting");
        auto conn0 = client->connect(yield[ec], shared->cancel);
        BOOST_TEST_REQUIRE(!ec, "Client connect with retries: " << ec.message());

        steady_clock::time_point conn0_end = steady_clock::now();

        BOOST_TEST_MESSAGE("Connection #0 established in " << as_seconds(conn0_end - conn0_start) << " seconds");

        namespace accu = boost::accumulators;
        accu::accumulator_set<float, accu::stats<accu::tag::mean, accu::tag::variance, accu::tag::min, accu::tag::max > > acc;

        for (unsigned i = 0; i < shared->subsequent_conn_count; i++) {

            steady_clock::time_point conn_start = steady_clock::now();
            auto conn = client->connect(yield[ec], shared->cancel);
            BOOST_TEST_REQUIRE(!ec, "Client connect with retries: " << ec.message());
            steady_clock::time_point conn_end = steady_clock::now();

            auto duration_s = as_seconds(conn_end - conn_start);
            BOOST_TEST_MESSAGE("Connection #" << (i+1) << " established in " << duration_s << " seconds");

            acc(duration_s);
        }

        std::cout << "Subsequent connections:\n";
        std::cout << "    Sample count:  " << accu::count(acc) << "\n";
        std::cout << "    mean:          " << accu::mean(acc) << "\n";
        std::cout << "    variance:      " << accu::variance(acc) << "\n";
        std::cout << "    std deviation: " << sqrt(accu::variance(acc)) << "\n";
        std::cout << "    min:           " << accu::min(acc) << "\n";
        std::cout << "    max:           " << accu::max(acc) << "\n";

        // Tell the server we're done.
        shared->client_finished_lock.release();
        shared->cancel();
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

