#define BOOST_TEST_MODULE i2p_tracker
#include <boost/test/included/unit_test.hpp>
#include <boost/filesystem.hpp>

#include <boost/asio/spawn.hpp>
#include <iostream>

#include <namespaces.h>
#include <bittorrent/bep3_tracker.h>
#include "util/test_dir.h"
#include <ouiservice/i2p.h>

using namespace ouinet;
using namespace bittorrent;
namespace test = boost::unit_test;
using I2pService = ouiservice::i2poui::Service;

void handle_exception(const char* actor, std::exception_ptr ep) {
    // NOTE: Don't re-throw from the `catch` blocks as that will cause the
    // `I2pService` to _not_ call its destructor which in turn will cause
    // the next test to fail at initialization.
    try {
        if (ep) std::rethrow_exception(ep);
    }
    catch (std::exception const& e) {
        BOOST_ERROR("Actor '" << actor << "' threw an exception: " << e.what());
    }
    catch (...) {
        BOOST_ERROR("Actor '" << actor << "' threw an unknown exception");
    }
}

BOOST_AUTO_TEST_CASE(announce_and_get_peers) {
    TestDir dir;

    asio::io_context ctx;

    const std::string tracker_id = "z2tfkf4t23gig3nfybnat2qarjl2f7dctcj63khfluqt2fdoikpa.b32.i2p";

    asio::spawn(ctx, [&] (asio::yield_context yield) mutable {
        sys::error_code ec;

        auto service = std::make_shared<I2pService>(dir.string(), yield.get_executor(), 1);
        auto server = service->build_server("key-file-name");

        server->start_listen(yield[ec]);
        BOOST_REQUIRE_MESSAGE(!ec, "Server failed on start_listen " << ec.message());

        auto dst = server->get_destination();

        BOOST_REQUIRE(dst);

        Bep3Tracker tracker(service, tracker_id, dst);

        auto infohash = NodeID::random();

        Cancel cancel;
        tracker.tracker_announce(infohash, cancel, yield[ec]);

        BOOST_REQUIRE_MESSAGE(!ec, "Announce failed with " << ec.message());
    },
    [] (auto e) { handle_exception("server", e); });

    ctx.run();
}

