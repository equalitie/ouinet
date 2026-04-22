#define BOOST_TEST_MODULE i2p_tracker
#include <boost/test/included/unit_test.hpp>
#include <boost/filesystem.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include <namespaces.h>
#include <bittorrent/bep3_tracker.h>
#include <ouiservice/i2p.h>
#include <task.h>
#include "util/test_dir.h"

using namespace ouinet;
using namespace bittorrent;
namespace test = boost::unit_test;

void handle_exception(std::exception_ptr ep) {
    try {
        if (ep) std::rethrow_exception(ep);
    }
    catch (std::exception const& e) {
        BOOST_ERROR("Exception: " << e.what());
    }
    catch (...) {
        BOOST_ERROR("Unknown exception");
    }
}

BOOST_AUTO_TEST_CASE(tracker_status) {
    TestDir dir;

    asio::io_context ctx;

    const auto tracker_id = *I2pAddress::parse("z2tfkf4t23gig3nfybnat2qarjl2f7dctcj63khfluqt2fdoikpa.b32.i2p");

    asio::spawn(ctx, [&] (asio::yield_context yield) mutable {
        Cancel cancel;
        sys::error_code ec;

        auto service = std::make_shared<I2pService>(dir.string(), yield.get_executor(), 1);
        auto client = service->build_client(tracker_id);

        client->start(yield[ec]);
        BOOST_TEST_REQUIRE(!ec, ec.message());

        auto conn = client->connect_without_handshake(yield[ec], cancel);
        BOOST_TEST_REQUIRE(!ec, "Client connect: " << ec.message());

        std::string target = "http://" + tracker_id.value + "/a";

        http::request<http::empty_body> request{http::verb::get, target, 11};
        request.set(http::field::host, tracker_id.value);
        request.set(http::field::user_agent, "Ouinet/1.0");

        http::async_write(conn, request, yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Client send: " << ec.message());

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::async_read(conn, buffer, response, yield[ec]);
        BOOST_TEST_REQUIRE(!ec, "Client receive: " << ec.message());
        BOOST_TEST_REQUIRE(response.result() == http::status::ok, "Response status: " << response.result());
    },
    handle_exception);

    ctx.run();
}

BOOST_AUTO_TEST_CASE(announce_and_get_peers) {
    TestDir dir;

    asio::io_context ctx;

    const auto tracker_id = *I2pAddress::parse("z2tfkf4t23gig3nfybnat2qarjl2f7dctcj63khfluqt2fdoikpa.b32.i2p");

    asio::spawn(ctx, [&] (asio::yield_context yield) mutable {
        sys::error_code ec;

        auto service = std::make_shared<I2pService>(dir.string(), yield.get_executor(), 1);

        Cancel stop_server;
        BOOST_REQUIRE(!ec);

        auto server = service->build_server("key-file-name");

        server->start_listen(yield[ec]);
        BOOST_REQUIRE_MESSAGE(!ec, "Server failed on start_listen " << ec.message());

        auto dst = server->get_destination();

        BOOST_REQUIRE(dst);

        Bep3Tracker client(*server, tracker_id);

        auto infohash = NodeID::random();

        Cancel cancel;
        client.tracker_announce(infohash, cancel, yield[ec]);
        BOOST_REQUIRE_MESSAGE(!ec, "Announce failed with " << ec.message());

        auto peers = client.tracker_get_peers(infohash, cancel, yield[ec]);
        BOOST_REQUIRE_MESSAGE(!ec, "Get peers failed with " << ec.message());

        BOOST_REQUIRE(peers.size() == 1);
        stop_server();
    },
    handle_exception);

    ctx.run();
}
