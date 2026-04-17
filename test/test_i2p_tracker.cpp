#define BOOST_TEST_MODULE i2p_tracker
#include <boost/test/included/unit_test.hpp>
#include <boost/filesystem.hpp>

#include <boost/asio/spawn.hpp>
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

BOOST_AUTO_TEST_CASE(announce_and_get_peers) {
    TestDir dir;

    asio::io_context ctx;

    const std::string tracker_id = "z2tfkf4t23gig3nfybnat2qarjl2f7dctcj63khfluqt2fdoikpa.b32.i2p";

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
