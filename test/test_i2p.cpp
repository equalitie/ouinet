#define BOOST_TEST_MODULE utility
#include <boost/test/unit_test.hpp>

#include "namespaces.h"
#include "util/wait_condition.h"
#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/address.h"
#include "task.h"
#include "util/async.h"
#include "util/random.h"
#include "util/promise.h"
#include "util/unwrap.h"

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/spawn.hpp>

#include <iostream>
#include <chrono>
#include <vector>
#include <queue>

using namespace ouinet;
using namespace std::chrono;
using namespace std::chrono_literals;
namespace test = boost::unit_test;
using tcp = asio::ip::tcp;

template<class Rep, class Period>
float as_seconds(std::chrono::duration<Rep, Period> duration) {
    return duration_cast<milliseconds>(duration).count() / 1000.f;
}

void handle_exception(const char* actor, std::exception_ptr ep) {
    try {
        if (ep) std::rethrow_exception(ep);
    }
    catch (std::exception const& e) {
        BOOST_ERROR("Actor '" << actor << "' threw an exception: " << e.what());
    }
}

template<class ServerJob, class ClientJob>
void run_two(asio::io_context& ctx, ServerJob server_job, ClientJob client_job)
{
    task::spawn_detached(ctx,
        [ server_job = std::move(server_job)
        , client_job = std::move(client_job)
        ] (asio::yield_context yield) mutable {
            WaitCondition server_finished(yield.get_executor());
            WaitCondition client_finished(yield.get_executor());

            auto test_name = boost::unit_test::framework::current_test_case().p_name;
            // Server
            asio::spawn(yield.get_executor(), [&, job = std::move(server_job), lock = server_finished.lock()] (asio::yield_context yield) mutable {
                    job(Async(
                        yield,
                        util::LogPath(test_name).tag("server")
                    ));
                },
                [] (auto e) { handle_exception("server", e); });

            // Client
            asio::spawn(yield.get_executor(), [&, job = std::move(client_job), lock = client_finished.lock()] (asio::yield_context yield) mutable {
                    job(Async(
                        yield,
                        util::LogPath(test_name).tag("client")
                    ));
                },
                [] (auto e) { handle_exception("client", e); });

            server_finished.wait(yield);
            client_finished.wait(yield);
    });

    ctx.run();
}

struct Handshake {
    static constexpr std::string_view request_msg = "request";
    static constexpr std::string_view reply_msg = "reply";
    
    // send request, receive reply
    static void client(tcp::socket& socket, Async yield) {
        unwrap(asio::async_write(socket, asio::buffer(request_msg), yield));
        std::string buffer_rx(reply_msg.size(), 'X');
        unwrap(asio::async_read(socket, asio::buffer(buffer_rx), yield));
        BOOST_REQUIRE_EQUAL(buffer_rx, reply_msg);
    }
    
    // receive request, send reply
    static void server(tcp::socket& socket, Async yield) {
        std::string buffer_rx(request_msg.size(), 'X');
        unwrap(asio::async_read(socket, asio::buffer(buffer_rx), yield));
        BOOST_REQUIRE_EQUAL(buffer_rx, request_msg);
        unwrap(asio::async_write(socket, asio::buffer(reply_msg), yield));
    }
};

// Create a connected socket pair and pass individual sockets to the corresponding jobs.
// I2P by its nature is less stable than local TCP, so re-trying is necessary for
// stable tests.
template<class ServerJob, class ClientJob>
void run_connected(asio::io_context& ctx, ServerJob server_job, ClientJob client_job) {
    struct SharedState {
        SharedState(asio::any_io_executor exec)
            : client_finished(exec)
            , server_finished(exec)
            , server_ep(exec)
        {}

        WaitCondition client_finished;
        WaitCondition server_finished;
        Promise<I2pAddress> server_ep;
    };

    auto shared = make_shared<SharedState>(ctx.get_executor());

    run_two(ctx,
        // Server
        [shared, lock = shared->server_finished.lock(), job = std::move(server_job)] (Async yield) mutable {
            auto session = unwrap(I2pSession::create(yield));
            shared->server_ep.set_value(session.local_addr());

            auto socket = unwrap(session.accept(yield));
            Handshake::server(socket, yield);

            lock.release();
            shared->client_finished.wait(yield);

            job(std::move(session), std::move(socket), yield);
        },
        // Client
        [shared, lock = shared->client_finished.lock(), job = std::move(client_job)] (Async yield) mutable {
            auto session = unwrap(I2pSession::create(yield));
            auto server_ep = *shared->server_ep.get_future().wait(yield);

            auto socket = unwrap(session.connect(server_ep, yield));
            Handshake::client(socket, yield);

            lock.release();
            shared->server_finished.wait(yield);

            job(std::move(session), std::move(socket), yield);
        });

    ctx.run();
}

std::vector<unsigned char> generate_random_bytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    util::random::data(bytes.data(), bytes.size());
    return bytes;
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
    asio::io_context ctx;

    struct SharedState {
        SharedState(asio::any_io_executor exec)
            : server_finished(exec)
        {}

        WaitCondition server_finished;
        steady_clock::time_point send_started;
        std::queue<std::vector<unsigned char>> sent_messages;
        const unsigned int buffer_size = 512;
        const unsigned int message_count = 5 * 1024 * 1024 / buffer_size;
    };

    auto shared = make_shared<SharedState>(ctx.get_executor());

    run_connected(ctx,
        // Server
        [shared, lock = shared->server_finished.lock()] (I2pSession session, tcp::socket socket, Async yield) mutable {
            std::vector<unsigned char> buffer(shared->buffer_size);

            for (uint32_t i = 0; i < shared->message_count; i++) {
                if (i % 512 == 0 && i != 0) {
                    BOOST_TEST_MESSAGE("Server received " << i << " out of "
                            << shared->message_count << " messages so far");
                }
                size_t size = unwrap(asio::async_read(socket, asio::buffer(buffer), yield));
                BOOST_REQUIRE_EQUAL(size, shared->buffer_size);

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
        },
        // Client
        [shared] (I2pSession session, tcp::socket socket, Async yield) mutable {
            shared->send_started = steady_clock::now();

            for (uint32_t i = 0; i < shared->message_count; i++) {
                shared->sent_messages.push(generate_random_bytes(shared->buffer_size));
                unwrap(asio::async_write(socket, asio::buffer(shared->sent_messages.back()), yield));
            }
            // Prevent session from being destroyed which would send EOF to the server.
            shared->server_finished.wait(yield);
        });

    ctx.run();
}

// Measure how long it takes for connections to be established to the same
// endpoint once one connection is already active.
BOOST_AUTO_TEST_CASE(test_subsequent_connection_speed) {
    asio::io_context ctx;

    struct SharedState {
        SharedState(asio::any_io_executor exec):
            server_ep(exec),
            server_finished(exec)
        {}

        Promise<I2pAddress> server_ep;
        const unsigned subsequent_conn_count = 32;
        const steady_clock::time_point conn0_start = steady_clock::now();
        WaitCondition server_finished;
    };

    auto shared = make_shared<SharedState>(ctx.get_executor());

    run_connected(ctx,
        // Server
        [ shared,
          lock = shared->server_finished.lock()
        ]
        (I2pSession session, tcp::socket socket_, Async yield) mutable {
            shared->server_ep.set_value(session.local_addr());
            for (unsigned i = 0; i < shared->subsequent_conn_count; i++) {
                assert(!yield.is_cancelled());
                auto socket = unwrap(session.accept(yield));
                Handshake::server(socket, yield);
                BOOST_TEST_MESSAGE("Server accept #" << (i+1));
            }
        },
        // Client
        [shared]
        (I2pSession session, tcp::socket socket_, Async yield) mutable {
            auto server_ep = *shared->server_ep.get_future().wait(yield);
            steady_clock::time_point conn0_end = steady_clock::now();

            BOOST_TEST_MESSAGE("Connection #0 established in "
                    << as_seconds(conn0_end - shared->conn0_start) << " seconds");

            namespace accu = boost::accumulators;

            accu::accumulator_set<float,
                accu::stats<
                    accu::tag::mean,
                    accu::tag::variance,
                    accu::tag::min,
                    accu::tag::max
                > > acc;

            for (unsigned i = 0; i < shared->subsequent_conn_count; i++) {
                assert(!yield.is_cancelled());

                auto conn_start = steady_clock::now();
                auto socket = unwrap(session.connect(server_ep, yield));

                Handshake::client(socket, yield);

                auto conn_end = steady_clock::now();

                auto duration_s = as_seconds(conn_end - conn_start);
                BOOST_TEST_MESSAGE("Connection #" << (i+1) << " established in "
                        << duration_s << " seconds");

                acc(duration_s);
            }

            std::cout << "Subsequent connections:\n";
            std::cout << "    Sample count:  " << accu::count(acc) << "\n";
            std::cout << "    mean:          " << accu::mean(acc) << "\n";
            std::cout << "    variance:      " << accu::variance(acc) << "\n";
            std::cout << "    std deviation: " << sqrt(accu::variance(acc)) << "\n";
            std::cout << "    min:           " << accu::min(acc) << "\n";
            std::cout << "    max:           " << accu::max(acc) << "\n";

            shared->server_finished.wait(yield);
        });

    ctx.run();
}
