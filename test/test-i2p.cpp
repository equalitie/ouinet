#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>
#include <boost/filesystem.hpp>

#include <boost/asio/spawn.hpp>
#include <iostream>
#include <chrono>

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

    struct State {
        State(const Setup& setup, AsioExecutor exec)
            : exec(exec)
            , service(make_shared<I2pOuiService>(setup.tempdir.string(), exec))
            , server_ready(exec)
            , client_finished(exec)
            , client_finished_lock(client_finished.lock())
            , work_guard(exec)
        {}

        AsioExecutor exec;
        shared_ptr<I2pOuiService> service;
        WaitCondition server_ready;
        WaitCondition client_finished;
        WaitCondition::Lock client_finished_lock;
        string server_ep;
        string message = "hello";
        // TODO: We should not need this here, the i2p service should take care of it.
        asio::executor_work_guard<AsioExecutor> work_guard;
    };

    auto state = make_shared<State>(setup, ctx.get_executor());

    // Server
    asio::spawn(ctx, [state, server_ready_lock = state->server_ready.lock()] (asio::yield_context yield) {
        auto server = state->service->build_server("i2p-private-key");

        state->server_ep = server->public_identity();

        sys::error_code ec;

        server->start_listen(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        server_ready_lock.release();

        GenericStream conn = server->accept(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());
        cout << "Server accepted connection\n";

        asio::async_write(conn, asio::buffer(state->message), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());
        cout << "Server sent \"" << state->message << "\"\n";

        state->client_finished.wait(yield);
    });


    // Client
    asio::spawn(ctx, [state = std::move(state)] (asio::yield_context yield) {
        cout << "Await server_ready (this may take a while)\n";
        state->server_ready.wait(yield);
        cout << "Server is ready\n";

        auto client = state->service->build_client(state->server_ep);

        sys::error_code ec;

        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        Cancel cancel;

        auto conn = client->connect(yield[ec], cancel);
        BOOST_TEST_REQUIRE(!ec, ec.message());
        cout << "Client connected\n";

        std::string buffer(state->message.size(), 'X');
        asio::async_read(conn, asio::buffer(buffer), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        cout << "Client read \"" << buffer << "\"\n";
        BOOST_REQUIRE_EQUAL(buffer, state->message);

        state->client_finished_lock.release();
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

