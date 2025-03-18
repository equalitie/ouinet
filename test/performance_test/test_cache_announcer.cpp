#include <iostream>
#include <chrono>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/signal_set.hpp>

#include "../test/util/bittorrent_utils.cpp"

#define private public
#include "cache/announcer.cpp"

using namespace chrono;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;
using namespace ouinet::cache;
using namespace ouinet::util;
using namespace std;

using Clock = chrono::steady_clock;

const size_t N_GROUPS = 128;
const size_t TEST_SIMULTANEOUS_ANNOUNCEMENTS = 64;

shared_ptr<MainlineDht> btdht;
std::unique_ptr<Announcer> announcer;
Clock::time_point start;
Clock::time_point now;

void start_btdht(asio::io_context& ctx, BtUtils& btu) {
    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        vector<asio::ip::address> ifaddrs{asio::ip::make_address("0.0.0.0")};
        btdht = btu.bittorrent_dht(yield, ifaddrs);
    });
}

void start_announcer_loop(asio::io_context& ctx) {
    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        announcer = std::make_unique<Announcer>(btdht, TEST_SIMULTANEOUS_ANNOUNCEMENTS);

        start = Clock::now();
        for (size_t n = 0; n < N_GROUPS; n++) {
            announcer->add("group-" + std::to_string(n));
        }
    });
}

void monitor_announcements(asio::io_context& ctx, BtUtils& btu) {
    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        using namespace std::chrono;
        sys::error_code ec;
        asio::steady_timer timer(ctx);
        size_t announcing_attempts{0};
        size_t last_announcing_attempts{0};

        while (announcing_attempts < N_GROUPS) {
            announcing_attempts = 0;
            for (auto iter = announcer->_loop->entries.begin(); iter != announcer->_loop->entries.end(); ++iter)
                if (iter->first.attempted_update())
                    ++announcing_attempts;

            timer.expires_after(chrono::seconds(1));
            timer.async_wait(yield[ec]);
            now = Clock::now();
            auto elapsed = duration_cast<seconds>(now - start).count();

            if (announcing_attempts > last_announcing_attempts) {
                last_announcing_attempts = announcing_attempts;
                std::cout << announcing_attempts << " of " << N_GROUPS << " entries announced after " \
                      << elapsed << " seconds" << std::endl;
            }
        }

        btu.stop();
        raise(SIGINT);
    });
}

int main(int argc, const char** argv)
{
    asio::io_context ctx;
    BtUtils btu{ctx};

    start_btdht(ctx, btu);
    start_announcer_loop(ctx);
    monitor_announcements(ctx, btu);

    boost::asio::signal_set signals(ctx, SIGINT);
    signals.async_wait([&](const boost::system::error_code& error , int signal_number) {
        ctx.stop();
    });

    ctx.run();
}
