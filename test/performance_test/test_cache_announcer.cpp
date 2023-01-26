#include "../test/util/bittorrent_utils.cpp"

#include "util/handler_tracker.h"
#include "cache/announcer.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;
using namespace ouinet::cache;
using namespace ouinet::util;

const size_t N_GROUPS=10;

shared_ptr<MainlineDht> btdht;
std::unique_ptr<Announcer> announcer;

void start_btdht(asio::io_context& ctx) {
    asio::spawn(ctx, [&] (asio::yield_context yield) {
        BtUtils btu{ctx};
        vector<asio::ip::address> ifaddrs{asio::ip::make_address("0.0.0.0")};
        btdht = std::move(btu.bittorrent_dht(yield, ifaddrs));
    });
}

void start_announcer_loop(asio::io_context& ctx) {
    asio::spawn(ctx, [&] (asio::yield_context yield) {
        announcer = std::make_unique<Announcer>(btdht);

        for (size_t n = 0; n < N_GROUPS; n++) {
            announcer->add("group-" + std::to_string(n));
        }
    });
}

int main(int argc, const char** argv)
{
    asio::io_context ctx;

    start_btdht(ctx);
    start_announcer_loop(ctx);
    // TODO: Monitor the status of the announcements
    // TODO: Trigger the cancel signal when the monitoring is done
    ctx.run();
}