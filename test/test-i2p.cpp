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

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

using namespace std;
using namespace ouinet;
using namespace chrono;
using namespace chrono_literals;
namespace test = boost::unit_test;
using ouiservice::I2pOuiService;
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

BOOST_AUTO_TEST_CASE(test_connect_and_exchage) {
    Setup setup;

    asio::io_context ctx;

    struct SharedState {
        SharedState(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<I2pOuiService>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        shared_ptr<I2pOuiService> service;
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

        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        GenericStream conn = server->accept(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept: " << ec.message());

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

        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        Cancel cancel;

        auto conn = client->connect(yield[ec], cancel);
        BOOST_TEST_REQUIRE(!ec, "Client connect: " << ec.message());

        std::string buffer(hello_message.size(), 'X');
        asio::async_read(conn, asio::buffer(buffer), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Client read: " << ec.message());

        BOOST_REQUIRE_EQUAL(buffer, hello_message);

        shared->client_finished_lock.release();
    });

    ctx.run();
}

GenericStream accept_with_retry(Server& server, Cancel cancel, asio::yield_context yield) {
    while (!cancel) {
        sys::error_code ec;

        auto cancelled = cancel.connect([&server] {
            server.stop_listen();
        });
    
        GenericStream conn = server.accept(yield[ec]);
    
        if (cancelled) {
            break;
        }
    
        BOOST_TEST_REQUIRE(!ec, "Server accept: " << ec.message());
    
        asio::async_write(conn, asio::buffer(hello_message), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server write: " << ec.message());

        std::string buffer(hello_message.size(), 'x');
        asio::async_read(conn, asio::buffer(buffer), yield[ec]);
        BOOST_TEST_WARN(!ec, "Server write: " << ec.message());

        if (ec) continue;

        BOOST_REQUIRE_EQUAL(buffer, hello_message);

        return conn;
    }

    return or_throw<GenericStream>(yield, asio::error::connection_aborted);
}

struct RetryingClient {
    std::shared_ptr<I2pOuiService> _service;
    std::unique_ptr<Client> _client = nullptr;

    GenericStream connect_with_retry(std::string server_ep, Cancel cancel, asio::yield_context yield) {
        unsigned retry_count = 32;
        unsigned retry_num = 0;
        
        while (true) {
            BOOST_TEST_REQUIRE(retry_num < retry_count);
        
            auto _client = _service->build_client(server_ep);
        
            sys::error_code ec;
        
            _client->start(yield[ec]);
            BOOST_TEST_WARN(!ec, "Client start: " << ec.message());
        
            if (ec) {
                retry_num += 1;
                continue;
            }
        
            auto conn = _client->connect(yield[ec], cancel);
            BOOST_TEST_WARN(!ec, "Client connect: " << ec.message());
        
            if (ec) {
                retry_num += 1;
                continue;
            }
        
            asio::async_write(conn, asio::buffer(hello_message), yield[ec]);
            BOOST_TEST_REQUIRE(!ec, "Client write: " << ec.message());

            std::string buffer(hello_message.size(), 'X');
            asio::async_read(conn, asio::buffer(buffer), yield[ec]);
            BOOST_TEST_WARN(!ec, "Client read: " << ec.message());
        
            if (ec) {
                retry_num += 1;
                continue;
            }
        
            BOOST_REQUIRE_EQUAL(buffer, hello_message);
        
            return conn;
        }
    }
};

BOOST_AUTO_TEST_CASE(test_connect_with_retry_and_exchage) {
    Setup setup;

    asio::io_context ctx;

    struct SharedState {
        SharedState(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<I2pOuiService>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        Cancel cancel;
        shared_ptr<I2pOuiService> service;
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

        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        accept_with_retry(*server, shared->cancel, yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept: " << ec.message());

        shared->client_finished.wait(yield);
    });


    // Client
    asio::spawn(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
        BOOST_TEST_MESSAGE("Client awaits server_ready (this may take a while)");
        shared->server_ready.wait(yield);
        BOOST_TEST_MESSAGE("Server is ready");

        RetryingClient client{shared->service};

        sys::error_code ec;
        client.connect_with_retry(shared->server_ep, shared->cancel, yield[ec]);
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
            , service(make_shared<I2pOuiService>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
        {}

        AsioExecutor exec;
        Cancel cancel;
        shared_ptr<I2pOuiService> service;
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

        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server start_listen: " << ec.message());

        server_ready_lock.release();

        auto conn = accept_with_retry(*server, shared->cancel, yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Server accept: " << ec.message());

        std::vector<unsigned char> buffer(shared->buffer_size);

        for (uint32_t i = 0; i < shared->message_count; i++) {
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

        RetryingClient client{shared->service};

        sys::error_code ec;
        auto conn = client.connect_with_retry(shared->server_ep, shared->cancel, yield[ec]);
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

BOOST_AUTO_TEST_SUITE_END()

