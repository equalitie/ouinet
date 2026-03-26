#define BOOST_TEST_MODULE ReachabilityAnalysis

#include <boost/asio.hpp>
#include <boost/test/included/unit_test.hpp>

#include "task.h"
#include "util/condition_variable.h"
#include "util/test_dir.h"

#define private public
#include "util/reachability.h"
#include "create_udp_multiplexer.h"

using namespace ouinet;
using namespace std;

namespace asio = boost::asio;

static string reachability_status(const util::UdpServerReachabilityAnalysis& reachability)
{
    switch (reachability.judgement())
    {
    case util::UdpServerReachabilityAnalysis::Reachability::Undecided:
        return "Undecided";
    case util::UdpServerReachabilityAnalysis::Reachability::ConfirmedReachable:
        return "Reachable";
    case util::UdpServerReachabilityAnalysis::Reachability::UnconfirmedReachable:
        return "UnconfirmedReachable";
    }
    assert(0 && "Invalid");
}

struct ReachabilityFixture
{
    asio::io_context ctx;
    const TestDir root;
    asio_utp::udp_multiplexer::endpoint_type endpoint;
    const unique_ptr<util::UdpServerReachabilityAnalysis> reachability_analysis = make_unique<
        util::UdpServerReachabilityAnalysis>();
    const uint8_t incoming_connections = 5;
    const uint8_t wait_for_unconfirmed_reachable= 1;


    ReachabilityFixture() = default;
    ~ReachabilityFixture() = default;

    void start_reachability_analysis()
    {
        task::spawn_detached(ctx, [&](const asio::yield_context&)
        {
            boost::optional<asio_utp::udp_multiplexer> mux = create_udp_multiplexer(
                ctx,
                root.path().string() + "/last_used_udp_port",
                0);
            BOOST_TEST(mux.has_value());
            endpoint = mux->local_endpoint();
            reachability_analysis->start(ctx.get_executor(), *mux);
        });
    }

    void simulate_incoming_connection()
    {
        task::spawn_detached(ctx, [&](const asio::yield_context&)
        {
            asio::ip::udp::socket socket(ctx);
            socket.open(asio::ip::udp::v4());
            const asio::ip::udp::endpoint dest(endpoint.address(), endpoint.port());

            auto msg = make_shared<string>("ABCDEabcde012345");
            socket.async_send_to(boost::asio::buffer(*msg), dest,
                                 [msg](const boost::system::error_code& ec, size_t bytes_sent)
                                 {
                                     if (ec) cerr << ec.message() << '\n';
                                 });
        });
    }
};

BOOST_FIXTURE_TEST_SUITE(suite_reachability, ReachabilityFixture);

    BOOST_AUTO_TEST_CASE(test_unconfirmed_reachable)
    {
        start_reachability_analysis();

        task::spawn_detached(ctx, [&](const asio::yield_context& yield)
        {
            BOOST_TEST(reachability_status(*reachability_analysis) == "Undecided");

            for (int i = 0; i < incoming_connections; ++i)
                simulate_incoming_connection();

            asio::steady_timer timer{ctx};
            timer.expires_after(chrono::seconds(wait_for_unconfirmed_reachable));
            timer.async_wait(yield);
            BOOST_TEST(reachability_status(*reachability_analysis) == "UnconfirmedReachable");

            reachability_analysis->stop();
        });
        ctx.run();
    }

BOOST_AUTO_TEST_SUITE_END();
