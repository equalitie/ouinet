#include "../test/util/i2p_utils.hpp"

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

BOOST_AUTO_TEST_CASE(test_connect_with_retry_and_exchange) {
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
    task::spawn_detached(ctx, [shared, server_ready_lock = shared->server_ready.lock()] (asio::yield_context yield) {
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
    task::spawn_detached(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
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

        BOOST_TEST_MESSAGE("waiting for 2 seconds to avoid a race condition");
        asio::steady_timer timer(shared->exec, asio::chrono::seconds(2));
        timer.async_wait(yield[ec]);

        // Tell the server we're done.
        shared->client_finished_lock.release();
        shared->cancel();
    });

    ctx.run();
}


BOOST_AUTO_TEST_SUITE_END()

