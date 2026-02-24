#include "../test/util/i2p_utils.hpp"

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

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
    task::spawn_detached(ctx, [shared = std::move(shared)] (asio::yield_context yield) {
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

BOOST_AUTO_TEST_SUITE_END()

