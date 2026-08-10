#define BOOST_TEST_MODULE i2p_tracker
#include <boost/test/unit_test.hpp>

#include <boost/filesystem.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast.hpp>

#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/tracker.h"
#include "util/unwrap.h"
#include "util/test_dir.h"
#include "util/log_path.h"
#include "util/async.h"
#include "util/i2p.h"
#include "util/wait_condition.h"
#include "bittorrent/node_id.h"
#include "namespaces.h"

using namespace ouinet;
using namespace bittorrent;
using namespace std::string_literals;
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

namespace std {
    template<class T>
    std::ostream& operator<<(std::ostream& os, std::optional<T> const& v) {
        if (v) {
            return os << *v;
        } else {
            return os << "none";
        }
    }
}

void spawn(auto& ctx, auto work) {
    asio::spawn(ctx, [work = std::move(work)] (asio::yield_context yield) mutable {
            // Wrap `asio::yield_context` in `Async` and pass `util::LogPath`
            // to it for convenient logging.
            work(
                Async(
                    yield,
                    util::LogPath(
                        boost::unit_test::framework::current_test_case().p_name
                    )
                )
            );
        },
        [] (std::exception_ptr ep) {
            // We don't expect exceptions, results from async actions using
            // `Async` are all of type `std::expected` and we explicitly check
            // their `.has_value()`.
            try {
                if (ep) std::rethrow_exception(ep);
            }
            catch (std::exception const& e) {
                BOOST_ERROR("Exception: " << e.what());
            }
            catch (...) {
                BOOST_ERROR("Unknown exception");
            }
        });
}

// vmon
static const auto tracker_id = *I2pAddress::parse("z2tfkf4t23gig3nfybnat2qarjl2f7dctcj63khfluqt2fdoikpa.b32.i2p");

BOOST_AUTO_TEST_CASE(tracker_status) {
    asio::io_context ctx;

    spawn(ctx, [&] (Async yield) mutable {
        auto i2pd = ensure_i2p_service(yield);

        auto session = unwrap(I2pSession::create(yield));

        auto socket = unwrap(session.connect(tracker_id, yield));

        BOOST_TEST_MESSAGE("Sending HTTP GET");

        http::request<http::empty_body> request{http::verb::get, "/", 11};
        request.set(http::field::host, "example.i2p");
        request.set(http::field::user_agent, "Ouinet/1.0");

        unwrap(http::async_write(socket, request, yield));

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        unwrap(http::async_read(socket, buffer, response, yield));

        BOOST_TEST_MESSAGE("Tracker response:\n" << response);
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(code) {
    auto b64 = "Qo~SVEYJFh5FohxpiIo8KGnj~OWPcbm1ETN-0U5hdhZCj9JURgkWHkWiHGmIijwoa"
               "eP85Y9xubURM37RTmF2FkKP0lRGCRYeRaIcaYiKPChp4~zlj3G5tREzftFOYXYWQo"
               "~SVEYJFh5FohxpiIo8KGnj~OWPcbm1ETN-0U5hdhZCj9JURgkWHkWiHGmIijwoaeP"
               "85Y9xubURM37RTmF2FkKP0lRGCRYeRaIcaYiKPChp4~zlj3G5tREzftFOYXYWQo~S"
               "VEYJFh5FohxpiIo8KGnj~OWPcbm1ETN-0U5hdhZCj9JURgkWHkWiHGmIijwoaeP85"
               "Y9xubURM37RTmF2FkKP0lRGCRYeRaIcaYiKPChp4~zlj3G5tREzftFOYXYWQo~SVE"
               "YJFh5FohxpiIo8KGnj~OWPcbm1ETN-0U5hdhZCj9JURgkWHkWiHGmIijwoaeP85Y9"
               "xubURM37RTmF2FnwQ-JVjbg9XyJpj4p9ELXcZIvqhD-Lw7o7DdtocrNQ9BQAEAAcA"
               "AA==";

    auto expected_b32 = "up476ksq55ggimakh45q2pjfaz4n5ptsjmxpsxcrebnhk72lgmrq"s += I2pAddress::B32::SUFFIX;

    auto calculated_b32 = unwrap(I2pAddress::B64::parse(b64)).to_b32();

    BOOST_REQUIRE_EQUAL(calculated_b32.as_str(), expected_b32);
}

BOOST_AUTO_TEST_CASE(announce_and_get_peers) {
    asio::io_context ctx;

    auto create_tracker = [] (Async yield) -> I2pTrackerClient {
        auto session = unwrap(I2pSession::create(yield));
        return I2pTrackerClient(std::make_shared<I2pSession>(std::move(session)), tracker_id);
    };

    spawn(ctx, [&] (Async yield) mutable {
        auto i2pd = ensure_i2p_service(yield);

        auto tracker_a = create_tracker(yield);
        auto tracker_b = create_tracker(yield);

        auto infohash = NodeID::random();

        auto a_b32_addr = tracker_a.get_session()->local_addr().to_b32();

        BOOST_TEST_MESSAGE("Announcing infohash " << infohash);

        unwrap(tracker_a.announce(infohash, yield));

        BOOST_TEST_MESSAGE("Retrieving peers");
        auto peers = unwrap(tracker_b.get_peers(infohash, yield));

        // We're announcing to a random infohash, so apart from us no one
        // should have announced to it. And the tracker won't send us our own
        // address.
        BOOST_REQUIRE_EQUAL(peers.size(), 1);

        auto peer = *peers.begin();
        BOOST_REQUIRE_EQUAL(peer, a_b32_addr);

        std::string message_tx = "hello world";

        WaitCondition wc(yield.get_executor());

        yield.spawn([&, lock = wc.lock()] (auto yield) {
            BOOST_TEST_MESSAGE("Server accepting");
            auto socket = unwrap(tracker_a.get_session()->accept(yield));
            std::string message_rx(message_tx.size(), '\0');
            BOOST_TEST_MESSAGE("Server reading message");
            unwrap(asio::async_read(socket, asio::buffer(message_rx), yield));
            BOOST_REQUIRE_EQUAL(message_tx, message_rx);
            BOOST_TEST_MESSAGE("Server done");
        });

        yield.spawn([&, s = tracker_b.get_session(), lock = wc.lock()] (auto yield) {
            BOOST_TEST_MESSAGE("Client connecting");
            auto socket = unwrap(s->connect(peer, yield));
            BOOST_TEST_MESSAGE("Client sending message");
            unwrap(asio::async_write(socket, asio::buffer(message_tx), yield));
            BOOST_TEST_MESSAGE("Client done");
        });

        wc.wait(yield);
    });

    ctx.run();
}
