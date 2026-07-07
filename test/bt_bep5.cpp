#include <bittorrent/mainline_dht.h>
#include <bittorrent/routing_table.h>

#include <boost/test/tools/old/interface.hpp>
#include <iostream>

#include <sys/types.h>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include "../src/constants.h"
#include "../src/bittorrent/mainline_dht.h"
#include "../src/bittorrent/dht_node.h"
#include "../src/task.h"
#include "../src/parse/number.h"
#include "../src/async_sleep.h"
#include "../src/util/compat.h"
#include "asio_utp/udp_multiplexer.hpp"
#include "progress.h"

using namespace ouinet;
using namespace std;
using namespace ouinet::bittorrent;
using boost::string_view;
using udp = asio::ip::udp;
using boost::optional;

void usage(std::ostream& os, const string& app_name, const char* what = nullptr) {
    if (what) {
        os << what << "\n" << endl;
    }

    os << "Usage:" << endl
       << "  " << app_name << " 0.0.0.0 [<ping>|<announce>|<get_peers>]" << endl;
}

template<size_t N>
static
boost::string_view as_string_view(const std::array<uint8_t, N>& a)
{
    return boost::string_view((char*) a.data(), a.size());
}

static
asio::ip::udp::endpoint parse_endpoint(const std::string& s)
{
    auto pos = s.find(':');
    assert(pos != s.npos);

    auto ip_s = s.substr(0, pos);
    auto port_s = s.substr(pos+1);
    auto ip = asio::ip::make_address(ip_s);
    boost::string_view port_sv(port_s);
    auto port = ouinet::parse::number<uint16_t>(port_sv);
    assert(port);
    return asio::ip::udp::endpoint(ip, *port);
}

void parse_args( const vector<string>& args
               , vector<asio::ip::address>* ifaddrs
               , bool* ping_cmd
               , bool* announce_cmd
               , bool* get_peers_cmd)
{
    if (args.size() == 2 && args[1] == "-h") {
        usage(std::cout, args[0]);
        exit(0);
    }

    if (args.size() < 3) {
        usage(std::cerr, args[0], "Too few arguments");
        exit(1);
    }

    if (args[2] == "ping") {
        *ping_cmd = true;
    }
    if (args[2] == "announce") {
        *announce_cmd = true;
    }
    if (args[2] == "get_peers") {
        *get_peers_cmd = true;
    }
}

void wait_for_ready(DhtNode& dht, asio::yield_context yield)
{
    auto ex = dht.get_executor();

    sys::error_code ec;
    Progress progress(ex, "Bootstrapping");

    compat([&](Async yield) { return dht.start(yield); })(yield[ec]);

    asio::steady_timer timer(ex);

    while (!ec && !dht.ready()) {
        timer.expires_after(chrono::milliseconds(200));
        timer.async_wait(yield[ec]);
    }
}

int main(int argc, const char** argv)
{
    asio::io_context ctx;

    auto metrics_client = metrics::Client::noop();
    auto metrics_dht = metrics_client.mainline_dht();
    auto dns_resolver = std::make_shared<dns::Resolver>();
    uint32_t rx_limit = udp_mux_rx_limit_client;

    asio_utp::udp_multiplexer socket(ctx.get_executor());
    sys::error_code ec;
    socket.bind({ asio::ip::address_v4::any(), 0 }, ec);
    if (ec) {
        cerr << "Error bind " << ec.message() << endl;
        return -1;
    }

    DhtNode dht(
        std::move(socket),
        metrics_dht.dht_node_ipv4(),
        dns_resolver,
        rx_limit,
        {},
        {},
        {}
    );

    vector<string> args;

    for (int i = 0; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    vector<asio::ip::address> ifaddrs;

    bool ping_cmd = false;
    bool announce_cmd = false;
    bool get_peers_cmd = false;

    parse_args(args, &ifaddrs, &ping_cmd, &announce_cmd, &get_peers_cmd);

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        wait_for_ready(dht, yield);

        cerr << "Our WAN endpoint: " << dht.wan_endpoint() << "\n";

        Cancel cancel;

        auto ep = resolve( udp::v4()
                         , "router.bittorrent.com"
                         , "6881"
                         , dns_resolver
                         , Async(yield));

        if (!ep) {
            cerr << "Error resolve " << ep.error().message() << endl;
            return;
        }

        sys::error_code ec;

        NodeID my_id = NodeID::generate(ep->address());

        if (ping_cmd) {
            NodeContact nc;

            if (args.size() == 5) {
                udp::endpoint peer_ep = parse_endpoint(args[3]);
                NodeID peer_id = *NodeID::from_hex(args[4]);
                nc = NodeContact{peer_id, peer_ep};
            }
            else {
                nc = NodeContact{my_id, *ep};
            }

            BencodedMap initial_ping_reply = compat([&](Async yield) {
                return dht.send_ping(nc, yield);
            })(cancel, yield[ec]);
            std::cout << initial_ping_reply << endl;
            if (!initial_ping_reply.empty()) {
                NodeID their_id = NodeID::from_bytestring(*((*initial_ping_reply["r"].as_map())["id"].as_string()));
                std::cout << their_id.to_hex() << endl;
                std::cout << "reply id == expected id: " << (my_id == their_id) << endl;
            }
        }

        if (announce_cmd) {
            if (args.size() < 4) {
                cerr << "Missing infohash argument\n";
                return;
            }

            NodeID infohash = *NodeID::from_hex(args[3]);

            auto peers = [&] {
                Progress p(ctx.get_executor(), "Announcing");
                return compat([&](Async yield) {
                    return dht.tracker_announce(infohash, std::nullopt, yield);
                })(cancel, yield[ec]);
            }();

            std::cout << "Found " << peers.size() << " peers\n";
        }

        if (get_peers_cmd) {
            if (args.size() < 4) {
                cerr << "Missing infohash argument\n";
                return;
            }

            auto infohash = *NodeID::from_hex(args[3]);

            auto peers = [&] {
                Progress p(ctx.get_executor(), "Getting peers");
                auto ps = compat([&](Async yield) {
                    return dht.tracker_get_peers(infohash, yield);
                })(cancel, yield[ec]);
                // Remove martian endpoints (cant't use `remove_if` on sets).
                for (auto it = ps.begin(); it != ps.end();)
                    if (!PeerFilter::martian.is_allowed(*it))
                        it = ps.erase(it);
                    else
                        it++;
                return ps;
            }();

            if (!ec) {
                cerr << "Found " << peers.size() << " peers:\n";
                for (auto i = peers.begin(); i != peers.end(); ++i) {
                    cerr << *i;
                    if (next(i) != peers.end()) cerr << ", ";
                }
                cerr << "\n";
            }
            else {
                cerr << "No peers found: " << ec.message() << "\n";
            }
        }

        cerr << "End" << endl;
        dht.stop();
    });

    ctx.run();
}
