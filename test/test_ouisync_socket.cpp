#define BOOST_TEST_MODULE test_ouisync_socket

#include <asio_utp/socket.hpp>
#include <asio_utp/udp_multiplexer.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/data/monomorphic.hpp>
#include <boost/test/unit_test.hpp>
#include <ouisync.hpp>
#include <ouisync/service.hpp>

#include "logger.h"
#include "ouiservice/ouisync/queue.h"
#include "ouiservice/ouisync/socket.h"
#include "util/promise.h"
#include "util/random.h"
#include "util/success_condition.h"
#include "util/wait_condition.h"

#include "util/async_test.h"
#include "util/test_dir.h"
#include "util/unwrap.h"

namespace asio = boost::asio;
namespace data = boost::unit_test::data;
using namespace ouinet;
using boost::asio::ip::udp;
using boost::system::error_code;

BOOST_AUTO_TEST_CASE(test_ping) {
    get_logger().set_threshold(DEBUG);

    async_test([](Async yield) {
        TestDir root;

        ouisync::init_log();

        ouisync::Service service(yield.get_executor());
        unwrap(service.start(root.string(), nullptr, yield));

        auto session = unwrap(ouisync::Session::connect(root.path(), yield));
        unwrap(session.bind_network({"quic/127.0.0.1:0"}, yield));

        auto alice_socket = unwrap(ouisync_service::OuisyncSocket::open(
            session,
            udp::v4(),
            yield
        ));

        auto bob_socket = udp::socket(
            yield.get_executor(),
            udp::endpoint(asio::ip::address_v4::loopback(), 0)
        );
        auto bob_endpoint = bob_socket.local_endpoint();

        const std::string ping("ping");
        const std::string pong("pong");

        // Send PING
        {
            auto buffer = asio::buffer(ping);
            error_code send_ec;
            size_t send_size = 0;

            WaitCondition wc(yield.get_executor());

            alice_socket.async_send_to(
                std::span(&buffer, 1),
                bob_endpoint,
                [&, lock = wc.lock()] (error_code ec, size_t size) {
                    send_ec = ec;
                    send_size = size;
                }
            );

            unwrap(wc.wait(yield));
            BOOST_REQUIRE_EQUAL(send_ec, error_code());
            BOOST_REQUIRE_EQUAL(send_size, 4);
        }

        // Receive PING, send PONG
        {
            udp::endpoint peer;
            std::string buffer_data(32, ' ');
            auto recv_size = unwrap(bob_socket.async_receive_from(
                asio::buffer(buffer_data),
                peer,
                yield
            ));
            BOOST_REQUIRE_EQUAL(recv_size, 4);
            BOOST_REQUIRE_EQUAL(buffer_data.substr(0, recv_size), ping);

            auto send_size = unwrap(bob_socket.async_send_to(
                asio::buffer(pong),
                peer,
                yield
            ));
            BOOST_REQUIRE_EQUAL(send_size, 4);
        }

        // Receive PONG
        {
            udp::endpoint peer;
            std::string buffer_data(32, ' ');
            auto buffer = asio::buffer(buffer_data);
            error_code recv_ec;
            size_t recv_size = 0;

            WaitCondition wc(yield.get_executor());

            alice_socket.async_receive_from(
                std::span(&buffer, 1),
                peer,
                [&, lock = wc.lock()] (error_code ec, size_t size) {
                    recv_ec = ec;
                    recv_size = size;
                }
            );

            unwrap(wc.wait(yield));
            BOOST_REQUIRE_EQUAL(recv_ec, error_code());
            BOOST_REQUIRE_EQUAL(recv_size, 4);
            BOOST_REQUIRE_EQUAL(buffer_data.substr(0, recv_size), pong);
        }
    });
}

enum class CancellationScope {
    object,
    operation
};

inline std::ostream& operator << (std::ostream& os, CancellationScope scope) {
    switch (scope) {
        case CancellationScope::object: return os << "object";
        case CancellationScope::operation: return os << "operation";
        default: throw std::invalid_argument("invalid cancellation scope");
    }
}

const auto cancellation_scopes = data::make({
    CancellationScope::object,
    CancellationScope::operation
});

BOOST_DATA_TEST_CASE(test_cancellation, cancellation_scopes, scope) {
    get_logger().set_threshold(DEBUG);

    async_test([=](Async yield) {
        TestDir root;

        ouisync::init_log();

        ouisync::Service service(yield.get_executor());
        unwrap(service.start(root.string(), nullptr, yield));

        auto session = unwrap(ouisync::Session::connect(root.path(), yield));
        unwrap(session.bind_network({"quic/127.0.0.1:0"}, yield));

        auto socket = unwrap(ouisync_service::OuisyncSocket::open(
            session,
            udp::v4(),
            yield
        ));

        std::vector<uint8_t> buffer_data;
        auto buffer = asio::buffer(buffer_data);
        udp::endpoint endpoint;

        WaitCondition wc(yield.get_executor());
        error_code op_ec;

        switch (scope) {
        case CancellationScope::object: {
            socket.async_receive_from(
                std::span(&buffer, 1),
                endpoint,
                [&op_ec, lock = wc.lock()] (error_code ec, size_t size) {
                    op_ec = ec;
                }
            );

            error_code cancel_ec;
            socket.cancel(cancel_ec);
            BOOST_REQUIRE(!cancel_ec);

            break;
        }
        case CancellationScope::operation: {
            asio::cancellation_signal signal;

            socket.async_receive_from(
                std::span(&buffer, 1),
                endpoint,
                asio::bind_cancellation_slot(
                    signal.slot(),
                    [&op_ec, lock = wc.lock()] (error_code ec, size_t size) {
                        op_ec = ec;
                    }
                )
            );

            signal.emit(asio::cancellation_type::total);

            break;
        }
        default:
            BOOST_FAIL("Invalid cancellation scope");
        }

        unwrap(wc.wait(yield));
        BOOST_REQUIRE_EQUAL(op_ec, asio::error::operation_aborted);
    });
}

BOOST_AUTO_TEST_CASE(test_utp) {
    get_logger().set_threshold(DEBUG);

    async_test([=](Async yield) {
        TestDir root;

        ouisync::init_log();

        auto request  = util::random::printable_ascii(1024 * 1024);
        auto response = util::random::printable_ascii(1024 * 1024);

        WaitCondition done(yield.get_executor());

        Promise<udp::endpoint> bob_endpoint(yield.get_executor());

        yield.tag("alice").spawn(
            [&, lock = done.lock(), bob_endpoint = bob_endpoint.get_future()]
            (Async yield) mutable {
                // Init Ouisync
                ouisync::Service service(yield.get_executor());
                unwrap(service.start(root.string(), nullptr, yield));

                auto session = unwrap(ouisync::Session::connect(root.path(), yield));
                unwrap(session.bind_network({"quic/127.0.0.1:0"}, yield));

                // Create uTP socket backed by  Ouisync's UDP socket
                auto ouisync_socket = unwrap(ouisync_service::OuisyncSocket::open(
                    session,
                    udp::v4(),
                    yield
                ));
                auto udp_socket = asio_utp::udp_multiplexer(yield.get_executor());
                udp_socket.bind(
                    std::make_unique<ouisync_service::OuisyncSocket>(
                        std::move(ouisync_socket)
                    )
                );
                auto utp_socket = asio_utp::socket(yield.get_executor());
                error_code ec;
                utp_socket.bind(std::move(udp_socket), ec);
                BOOST_REQUIRE(!ec);

                // asio_utp::socket utp_socket(yield.get_executor());
                // error_code ec;
                // utp_socket.bind({ asio::ip::address_v4::loopback(), 0 }, ec);
                // BOOST_REQUIRE(!ec);

                // Connect to the peer
                utp_socket
                    .async_connect(bob_endpoint.wait(yield).value(), yield)
                    .value();

                // Send request
                auto n = unwrap(asio::async_write(
                    utp_socket,
                    asio::buffer(request),
                    yield
                ));
                BOOST_REQUIRE_EQUAL(n, request.size());

                // Receive response
                std::string buffer;
                unwrap(asio::async_read(
                    utp_socket,
                    asio::dynamic_buffer(buffer, response.size()),
                    yield
                ));
                BOOST_REQUIRE_EQUAL(buffer.size(), response.size());
                BOOST_REQUIRE(buffer == response);
            }
        );

        yield.tag("bob").spawn(
            [&, lock = done.lock()]
            (Async yield) mutable {
                // Create uTP socket backed by regular UDP socket
                asio_utp::socket utp_socket(yield.get_executor());
                error_code ec;
                utp_socket.bind({ asio::ip::address_v4::loopback(), 0 }, ec);
                BOOST_REQUIRE(!ec);

                bob_endpoint.set_value(utp_socket.local_endpoint());

                // Accept peer connection
                utp_socket.async_accept(yield).value();

                // Receive request
                std::string buffer;
                unwrap(asio::async_read(
                    utp_socket,
                    asio::dynamic_buffer(buffer, request.size()),
                    yield
                ));
                BOOST_REQUIRE_EQUAL(buffer.size(), request.size());
                BOOST_REQUIRE(buffer == request);

                // Send response
                auto n = unwrap(asio::async_write(
                    utp_socket,
                    asio::buffer(response),
                    yield
                ));
                BOOST_REQUIRE_EQUAL(n, response.size());

                // Wait for the peer to receive the response. Not doing this could lead to the some
                // data not being received by the peer due to the sending socket being closed too
                // soon.
                lock.release();
                unwrap(done.wait(yield));
            }
        );

        unwrap(done.wait(yield));
    });
}

// Sanity check for the internal async queue used by `OuisyncSocket`.
BOOST_AUTO_TEST_CASE(test_queue) {
    get_logger().set_threshold(DEBUG);

    async_test([&] (Async yield) {
        ouisync_service::detail::Queue queue(yield.get_executor(), 2);
        SuccessCondition sc(yield.get_executor());

        yield.spawn([&, lock = sc.lock()] (Async yield) {
            auto [ ep, data ] = unwrap(queue.async_pop(yield));
            std::string payload(data.begin(), data.end());
            BOOST_REQUIRE_EQUAL(payload, "hello");
            lock.release(true);
        });

        std::string payload("hello");
        std::vector<uint8_t> data(payload.begin(), payload.end());
        auto pushed = queue.try_push(sys::error_code(), { udp::endpoint(), std::move(data) });
        BOOST_REQUIRE(pushed);

        bool success = sc.wait_for_success(yield);
        BOOST_REQUIRE(success);
    });
}
