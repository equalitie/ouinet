#include <bittorrent/dht.h>
#include <bittorrent/routing_table.h>

#include <iostream>

#include <sys/types.h>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include "../src/util/crypto.h"
#include "../src/parse/number.h"

using namespace ouinet;
using namespace std;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;
using boost::string_view;
using udp = asio::ip::udp;
using boost::optional;

void usage(std::ostream& os, const string& app_name, const char* what = nullptr) {
    if (what) {
        os << what << "\n" << endl;
    }

    os << "Usage:" << endl
       << "  " << app_name << " 0.0.0.0      [<get>|<put>|<ping>|<find_node>|<get_peers>]" << endl;

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
               , bool* find_node_cmd
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
    if (args[2] == "find_node") {
        *find_node_cmd = true;
    }
    if (args[2] == "get_peers") {
        *get_peers_cmd = true;
    }
}

int main(int argc, const char** argv)
{
    asio::io_service ios;

    DhtNode dht_ {ios, asio::ip::make_address("0.0.0.0")};

    vector<string> args;

    for (int i = 0; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    vector<asio::ip::address> ifaddrs;

    bool ping_cmd = false;
    bool find_node_cmd = false;
    bool get_peers_cmd = false;

    parse_args(args, &ifaddrs, &ping_cmd, &find_node_cmd, &get_peers_cmd);

    asio::spawn(ios, [&] (asio::yield_context yield) {
        sys::error_code ec;

        dht_.start(yield[ec]);

        asio::steady_timer timer(ios);

        while (!dht_.ready() && !ec) {
            cerr << "Not ready yet..." << endl;
            timer.expires_from_now(chrono::seconds(1));
            timer.async_wait(yield[ec]);
        }

        if (ec) {
            cerr << "Error timer.async_wait " << ec.message() << endl;
            return;
        }

        cerr << "Start" << endl;

        Cancel cancel;

        auto ep = resolve( ios
                         , "router.bittorrent.com"
                         , "6881"
                         , yield[ec]
                         , cancel);

        if (ec) {
            cerr << "Error resolve " << ec.message() << endl;
            return;
        }

        NodeID nid = NodeID::generate(ep.address());

        if (ping_cmd) {
            NodeContact nc;

            if (args.size() == 5) {
                udp::endpoint peer_ep = parse_endpoint(args[3]);
                NodeID peer_id = NodeID::from_hex(args[4]);
                nc = NodeContact{peer_id, peer_ep};
            }
            else {
                nc = NodeContact{nid, ep};
            }

            BencodedMap initial_ping_reply = dht_.send_ping(nc, yield[ec], cancel);
            std::cout << initial_ping_reply << endl;
            if (!initial_ping_reply.empty()) {
                NodeID their_id = NodeID::from_bytestring(*((*initial_ping_reply["r"].as_map())["id"].as_string()));
                std::cout << their_id.to_hex() << endl;
                std::cout << "reply id == expected id: " << (nid == their_id) << endl;
            }
        }

        if (find_node_cmd) {
            Contact nc {ep, nid};

            std::vector<ouinet::bittorrent::dht::NodeContact> v = {};
            bool is_found = dht_.query_find_node(nid, nc, v, yield[ec], cancel);

            std::cout << is_found << endl;
            for (const auto& i: v) {
                std::cout << i << ' ';
            }
            std::cout << endl;
        }

        if (get_peers_cmd) {
            Contact nc {ep, nid};

            std::vector<ouinet::bittorrent::dht::NodeContact> v = {};
            boost::optional<BencodedMap> peers = dht_.query_get_peers(nid, nc, v, yield[ec], cancel);

            if (peers) {
                std::cout << "Nodes: " << (*peers)["nodes"] << endl;
                std::cout << "Token: " << (*peers)["token"] << endl;
            }
        }

        cerr << "End" << endl;
    });

    ios.run();
}
