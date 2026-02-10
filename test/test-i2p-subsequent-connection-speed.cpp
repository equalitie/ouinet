#include "../test/util/i2p_utils.hpp"

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

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

