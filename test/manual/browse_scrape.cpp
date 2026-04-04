#include <boost/beast/core.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/stacktrace.hpp>
#include <boost/system/result.hpp>
#include <boost/program_options.hpp>
#include <namespaces.h>
#include <iostream>
#include <chrono>
#include "../util/test_dir.h"
#include "bittorrent/mock_dht.h"
#include "injector.h"
#include "client.h"
#include "util/str.h"
#include "../util/process.h"

using namespace std;
using namespace ouinet;
using namespace std::chrono_literals;
using bittorrent::MockDht;
namespace po = boost::program_options;

template<class Config>
static Config make_config(const std::vector<std::string>& args) {
    static constexpr auto c_str = [](const std::string& str) {
        return str.c_str();
    };

    std::vector<const char*> argv;
    std::transform(args.begin(), args.end(), std::back_inserter(argv), c_str);
    return Config(argv.size(), argv.data());
}

struct Options {
    std::string group;
    fs::path scrape_path;

    static Options parse(int argc, char* argv[]) {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("scrape", po::value<fs::path>(), "Path to a tar compressed scrape file")
            ("group", po::value<std::string>(), "DHT group");

        po::variables_map vm;        
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm); 

        if (!vm.count("scrape")) {
            std::cerr << "error: provide --scrape argument\n";
            std::cerr << desc << "\n";
            exit(1);
        }

        if (!vm.count("group")) {
            std::cerr << "error: provide --group argument\n";
            std::cerr << desc << "\n";
            exit(1);
        }

        auto scrape_path = vm["scrape"].as<fs::path>();
        auto group = vm["group"].as<std::string>();
        return Options {group, scrape_path};
    }
};

int main(int argc, char* argv[]) {
    auto opt = Options::parse(argc, argv);

    asio::io_context ctx;

    auto root = TestDir::Builder()
        .delete_if_exists(true)
        .delete_on_exit(true)
        .build("/tmp/ouinet/test-scrape");

    auto repo_dir = root.make_subdir("client");

    auto swarms = std::make_shared<MockDht::Swarms>();

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        test::util::execute(
            yield.get_executor(),
            "tar", {
                "xf"s,
                opt.scrape_path.string(),
                "-C"s, repo_dir.path().string()
            },
            yield);

        Client client(ctx, make_config<ClientConfig>({
                "./no_client_exec"s,
                "--log-level=DEBUG"s,
                "--repo"s, repo_dir.string(),

                "--cache-type=bep5-http"s,
                "--cache-http-public-key=zh6ylt6dghu6swhhje2j66icmjnonv53tstxxvj6acu64sc62fnq"s,

                "--listen-on-tcp=127.0.0.1:0"s,
                "--front-end-ep=127.0.0.1:0"s,

                "--add-request-field=X-Ouinet-Group:"s + opt.group,

                "--disable-origin-access"s,
                "--disable-injector-access",
                "--disable-proxy-access",
            }),
            util::LogPath("client"),
            [&ctx, swarms] () {
                return std::make_shared<MockDht>("client", ctx.get_executor(), swarms);
            });

        // Clients are started explicitly
        client.start();

        test::util::start_browser_for_client(yield.get_executor(), client, yield);

        client.stop();
    },
    [] (std::exception_ptr e) {
        if (e) std::rethrow_exception(e);
    });

    try {
        ctx.run();
    }
    catch (std::exception const& e) {
        cerr << "Exit with exception: " << e.what() << "\n";
        return 1;
    }
    catch (...) {
        cerr << "Exit with an exception.\n";
        return 1;
    }

    return 0;
}
