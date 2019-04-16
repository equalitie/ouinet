#include <bittorrent/dht.h>

#include <iostream>
#include <chrono>

#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <boost/asio/ip/address.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include "../src/cache/descidx.h"
#include "../src/util/crypto.h"
#include "../src/util/wait_condition.h"
#include "../src/util.h"


using namespace ouinet;
using namespace std;
using namespace ouinet::bittorrent;
using boost::string_view;
using udp = asio::ip::udp;
using boost::optional;

chrono::steady_clock::time_point now()
{
    return chrono::steady_clock::now();
}

float secs(std::chrono::steady_clock::duration d)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(d).count() / 1000.f;
}

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
       << "  " << app_name << " all          [<get>|<put>|<stress>] # All non loopback interfaces" << endl
       << "  " << app_name << " 0.0.0.0      [<get>|<put>|<stress>] # Any ipv4 interface" << endl
       << "  " << app_name << " 192.168.0.1  [<get>|<put>|<stress>] # Concrete interface" << endl
       << "Where:" << endl
       << "  <get>:    get <public-key> <dht-key>" << endl
       << "  <put>:    put <private-key> <dht-key> <dht-value>" << endl
       << "  <stress>: mulput <private-key>" << endl;

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

struct StressCmd {
    util::Ed25519PrivateKey private_key;
};

void parse_args( const vector<string>& args
               , vector<asio::ip::address>* ifaddrs
               , optional<GetCmd>* get_cmd
               , optional<PutCmd>* put_cmd
               , optional<StressCmd>* stress_cmd)
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
    if (args[2] == "stress") {
        if (args.size() != 4) {
            usage(std::cerr, args[0]);
            exit(1);
        }
        StressCmd c;
        c.private_key = *util::Ed25519PrivateKey::from_hex(args[3]);
        *stress_cmd = move(c);
    }
}

int main(int argc, const char** argv)
{
    asio::io_service ios;

    unique_ptr<MainlineDht> dht(new MainlineDht(ios));

    vector<string> args;

    for (int i = 0; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    vector<asio::ip::address> ifaddrs;

    optional<GetCmd>    get_cmd;
    optional<PutCmd>    put_cmd;
    optional<StressCmd> stress_cmd;

    parse_args(args, &ifaddrs, &get_cmd, &put_cmd, &stress_cmd);

    for (auto addr : ifaddrs) {
        std::cout << "Spawning DHT node on " << addr << std::endl;
    }

    dht->set_interfaces(ifaddrs);

    asio::spawn(ios, [&] (asio::yield_context yield) {
        using namespace std::chrono;

        sys::error_code ec;
        asio::steady_timer timer(ios);

        while (!dht->all_ready() && !ec) {
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

        steady_clock::time_point start = steady_clock::now();

        if (get_cmd) {
            auto salt = ouinet::util::sha1(get_cmd->dht_key);

            auto opt_data = dht->mutable_get( get_cmd->public_key
                                           , as_string_view(salt)
                                           , yield[ec], cancel);

            if (ec) {
                cerr << "Error dht->mutable_get " << ec.message() << endl;
            }
            else {
                if (opt_data) {
                    cerr << "Got Data!" << endl;
                    cerr << "seq:   " << opt_data->sequence_number << endl;
                    // src/cache/descidx.h
                    auto desc_str = [&]() {
                        auto val = *opt_data->value.as_string();
                        if (val.substr(0, ouinet::descriptor::zlib_prefix.size()) == ouinet::descriptor::zlib_prefix) {
                            auto desc_zlib(val.substr(ouinet::descriptor::zlib_prefix.length()));
                            return "zlib: " + util::zlib_decompress(desc_zlib, ec);
                        }
                        else {
                            return val;
                        }
                    }();
                    if (ec) {
                        cerr << "Error: dht->mutable_get: decoding value: " << ec.message() << endl;
                    }
                    cerr << "value: " << desc_str << endl;
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

            dht->mutable_put(item, cancel, yield[ec]);

            if (ec) {
                cerr << "FINISH: Error " << ec.message() << ", took:" << secs(now() - start) << "s\n";
            }
            else {
                cerr << "FINISH: Success, took:" << secs(now() - start) << "s\n";
            }
        }

        if (stress_cmd) {
            WaitCondition wc(ios);

            std::srand(std::time(nullptr));

            string key_base = util::str("ouinet-stress-test-", std::rand());

            for (unsigned int i = 0; i < 32; ++i) {
                asio::spawn(ios, [&, i, lock = wc.lock()](asio::yield_context yield) {
                    steady_clock::time_point start = steady_clock::now();

                    auto key = util::str(key_base, "-", i, "-key");
                    auto val = util::str(key_base, "-", i, "-val");

                    auto salt = ouinet::util::sha1(key);

                    using Time = boost::posix_time::ptime;
                    Time unix_epoch(boost::gregorian::date(1970, 1, 1));
                    Time ts = boost::posix_time::microsec_clock::universal_time();

                    auto seq = (ts - unix_epoch).total_milliseconds();

                    cerr << "seq: " << seq << endl;

                    auto item = MutableDataItem::sign( val
                                                     , seq
                                                     , as_string_view(salt)
                                                     , stress_cmd->private_key);

                    dht->mutable_put(item, cancel, yield[ec]);

                    if (ec) {
                        cerr << "FINISH" << i << ": Error " << ec.message() << ", took:" << secs(now() - start) << "s\n";
                    }
                    else {
                        cerr << "FINISH" << i << ": Success, took:" << secs(now() - start) << "s\n";
                    }
                });
            }

            wc.wait(yield);
        }

        cerr << "End. Took " << secs(now() - start) << " seconds" << endl;

        dht.reset();
    });

    ios.run();
}
