#define BOOST_TEST_MODULE test_ouisync_socket
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/data/monomorphic.hpp>

#include <boost/asio/ip/address_v4.hpp>
#include <ouisync.hpp>
#include <ouisync/service.hpp>

#include "logger.h"
#include "ouiservice/ouisync/socket.h"
#include "util/wait_condition.h"

#include "util/async_test.h"
#include "util/test_dir.h"
#include "util/unwrap.h"

namespace asio = boost::asio;
using boost::asio::ip::udp;
using boost::system::error_code;
using namespace ouinet;

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

const auto cancellation_scopes = boost::unit_test::data::make({
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
