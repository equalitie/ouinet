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
        string message = "hello";
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

        asio::async_write(conn, asio::buffer(shared->message), yield[ec]);
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

        std::string buffer(shared->message.size(), 'X');
        asio::async_read(conn, asio::buffer(buffer), yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Client read: " << ec.message());

        BOOST_REQUIRE_EQUAL(buffer, shared->message);

        shared->client_finished_lock.release();
    });

    ctx.run();
}


BOOST_AUTO_TEST_SUITE_END()

