#include "../test/util/i2p_utils.hpp"
#include <boost/asio/spawn.hpp>
#include <cstdio>

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

BOOST_AUTO_TEST_CASE(test_connect_and_exchange) {
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

    printf("scheduling spawning the server\n");
    // Server
    asio::spawn(ctx, [shared, server_ready_lock = shared->server_ready.lock()] (asio::yield_context yield) {
        printf("hello ffs");
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
    // asio::spawn(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
    //     BOOST_TEST_MESSAGE("Await server_ready (this may take a while)");
    //     shared->server_ready.wait(yield);
    //     BOOST_TEST_MESSAGE("Server is ready");

    //     auto client = shared->service->build_client(shared->server_ep);

    //     sys::error_code ec;

    //     BOOST_TEST_MESSAGE("Client starting");
    //     client->start(yield[ec]);
    //     BOOST_TEST_REQUIRE(!ec, ec.message());

    //     Cancel cancel;

    //     BOOST_TEST_MESSAGE("Client connecting");
    //     auto conn = client->connect_without_handshake(yield[ec], cancel);
    //     BOOST_TEST_REQUIRE(!ec, "Client connect: " << ec.message());

    //     BOOST_TEST_MESSAGE("Client reading hello message");
    //     std::string buffer(hello_message.size(), 'X');
    //     asio::async_read(conn, asio::buffer(buffer), yield[ec]);
    //     BOOST_TEST_REQUIRE(!ec, "Client read: " << ec.message());

    //     BOOST_REQUIRE_EQUAL(buffer, hello_message);

    //     shared->client_finished_lock.release();
    // });

    // ensure that we actually have things to run
    BOOST_TEST_REQUIRE(!ctx.poll());

    printf("running context\n");
    ctx.run();
    printf("finished running context\n");
}

BOOST_AUTO_TEST_SUITE_END()
