#include <bittorrent/dht.h>
#include <bittorrent/routing_table.h>

#include <iostream>

#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <boost/asio/ip/address.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include "../src/util/crypto.h"

using namespace ouinet;
using namespace std;
using namespace ouinet::bittorrent;
using namespace ouinet::bittorrent::dht;
using boost::string_view;
using udp = asio::ip::udp;
using boost::optional;

std::vector<asio::ip::address> linux_get_addresses()
{
    std::vector<asio::ip::address> output;

    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs)) {
        exit(1);
    }

    for (struct ifaddrs* ifaddr = ifaddrs; ifaddr != nullptr; ifaddr = ifaddr->ifa_next) {
        if (!ifaddr->ifa_addr) {
            continue;
        }

        if (ifaddr->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)ifaddr->ifa_addr;
            asio::ip::address_v4 ip(ntohl(addr->sin_addr.s_addr));
            output.push_back(ip);
        } else if (ifaddr->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* addr = (struct sockaddr_in6*)ifaddr->ifa_addr;
            std::array<unsigned char, 16> address_bytes;
            memcpy(address_bytes.data(), addr->sin6_addr.s6_addr, address_bytes.size());
            asio::ip::address_v6 ip(address_bytes, addr->sin6_scope_id);
            output.push_back(ip);
        }
    }

    freeifaddrs(ifaddrs);

    // TODO: filter unroutable addresses
    return output;
}

std::vector<asio::ip::address>
filter( bool loopback
      , bool ipv4
      , bool ipv6
      , const std::vector<asio::ip::address>& ifaddrs)
{
    std::vector<asio::ip::address> ret;

    for (auto addr : ifaddrs) {
        if (addr.is_loopback() && !loopback) continue;
        if (addr.is_v4()       && !ipv4)     continue;
        if (addr.is_v6()       && !ipv6)     continue;
        ret.push_back(addr);
    }

    return ret;
}

void usage(std::ostream& os, const string& app_name, const char* what = nullptr) {
    if (what) {
        os << what << "\n" << endl;
    }

    os << "Usage:" << endl
       << "  " << app_name << " [interface-address]" << endl
       << "E.g.:" << endl
       << "  " << app_name << " all          [<get>|<put>] # All non loopback interfaces" << endl
       << "  " << app_name << " 0.0.0.0      [<get>|<put>|<ping>] # Any ipv4 interface" << endl
       << "  " << app_name << " 192.168.0.1  [<get>|<put>] # Concrete interface" << endl
       << "Where:" << endl
       << "  <get>: get <public-key> <dht-key>" << endl
       << "  <put>: put <private-key> <dht-key> <dht-value>" << endl;

}

template<size_t N>
static
boost::string_view as_string_view(const std::array<uint8_t, N>& a)
{
    return boost::string_view((char*) a.data(), a.size());
}

struct GetCmd {
    util::Ed25519PublicKey public_key;
    string dht_key;
};

struct PutCmd {
    util::Ed25519PrivateKey private_key;
    string dht_key;
    string dht_value;
};

void parse_args( const vector<string>& args
               , vector<asio::ip::address>* ifaddrs
               , optional<GetCmd>* get_cmd
               , optional<PutCmd>* put_cmd)
{
    if (args.size() == 2 && args[1] == "-h") {
        usage(std::cout, args[0]);
        exit(0);
    }

    if (args.size() < 3) {
        usage(std::cerr, args[0], "Too few arguments");
        exit(1);
    }

    if (args[1] == "all") {
        *ifaddrs = filter( false
                         , true
                         , true
                         , linux_get_addresses());
    } else {
        boost::system::error_code ec;
        ifaddrs->push_back(asio::ip::make_address(args[1], ec));

        if (ec) {
            std::cerr << "Failed parsing \"" << args[1] << "\" as an IP "
                      << "address: " << ec.message() << std::endl;
            usage(std::cerr, args[0], "Failed to parse local endpoint");
            exit(1);
        }
    }

    if (args[2] == "get") {
        if (args.size() != 5) {
            usage(std::cerr, args[0]);
            exit(1);
        }
        GetCmd c;
        c.public_key = *util::Ed25519PublicKey::from_hex(args[3]);
        c.dht_key = args[4];
        *get_cmd = move(c);
    }
    if (args[2] == "put") {
        if (args.size() != 6) {
            usage(std::cerr, args[0]);
            exit(1);
        }
        PutCmd c;
        c.private_key = *util::Ed25519PrivateKey::from_hex(args[3]);
        c.dht_key = args[4];
        c.dht_value = args[5];
        *put_cmd = move(c);
    }
}

int main(int argc, const char** argv)
{
    asio::io_service ios;

    MainlineDht dht(ios);

    DhtNode dht_ {ios, asio::ip::make_address("0.0.0.0")};

    vector<string> args;

    for (int i = 0; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    vector<asio::ip::address> ifaddrs;

    optional<GetCmd> get_cmd;
    optional<PutCmd> put_cmd;

    parse_args(args, &ifaddrs, &get_cmd, &put_cmd);

    // for (auto addr : ifaddrs) {
    //     std::cout << "Spawning DHT node on " << addr << std::endl;
    // }

    // dht.set_interfaces(ifaddrs);

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

        {
            auto ping_ep = resolve(
                ios,
                "router.bittorrent.com",
                "6881",
                yield[ec],
                cancel
                );
            NodeID nid = NodeID::generate(ping_ep.address());
            NodeContact nc {nid, ping_ep};

            BencodedMap initial_ping_reply = dht_.send_ping(nc, yield[ec], cancel);
            std::cout << initial_ping_reply << endl;
            if (!initial_ping_reply.empty()) {
                NodeID their_id = NodeID::from_bytestring(*((*initial_ping_reply["r"].as_map())["id"].as_string()));
                std::cout << their_id.to_hex() << endl;
                std::cout << "reply id == expected id: " << (nid == their_id) << endl;
            }
        }

        if (get_cmd) {
            auto salt = ouinet::util::sha1(get_cmd->dht_key);

            auto opt_data = dht.mutable_get( get_cmd->public_key
                                           , as_string_view(salt)
                                           , yield[ec], cancel);

            if (ec) {
                cerr << "Error dht.mutable_get " << ec.message() << endl;
            }
            else {
                if (opt_data) {
                    cerr << "Got Data!" << endl;
                    cerr << "seq:   " << opt_data->sequence_number << endl;
                    cerr << "value: " << opt_data->value << endl;
                }
                else {
                    cerr << "No error, but also no data!" << endl;
                }
            }
        }

        if (put_cmd) {
            auto salt = ouinet::util::sha1(put_cmd->dht_key);

            using Time = boost::posix_time::ptime;
            Time unix_epoch(boost::gregorian::date(1970, 1, 1));
            Time ts = boost::posix_time::microsec_clock::universal_time();

            auto seq = (ts - unix_epoch).total_milliseconds();

            cerr << "seq: " << seq << endl;

            auto item = MutableDataItem::sign( put_cmd->dht_value
                                             , seq
                                             , as_string_view(salt)
                                             , put_cmd->private_key);

            dht.mutable_put(item, cancel, yield[ec]);

            if (ec) {
                cerr << "Error dht.mutable_put " << ec.message() << endl;
            }
        }

        cerr << "End" << endl;
    });

    ios.run();
}
