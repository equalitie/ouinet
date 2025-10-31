#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <iostream>
#include <chrono>
#include "util/test_dir.h"
#include "injector.h"
#include "client.h"

BOOST_AUTO_TEST_SUITE(ouinet_integration_tests)

using namespace std;
using namespace ouinet;
using namespace std::chrono_literals;

template<class Config>
static Config make_config(const std::vector<std::string>& args) {
    static constexpr auto c_str = [](const std::string& str) {
        return str.c_str();
    };

    std::vector<const char*> argv;
    std::transform(args.begin(), args.end(), std::back_inserter(argv), c_str);
    return Config(argv.size(), argv.data());
}

// WIP: Currently just creates an injector and a client and waits for them to bootstrap
BOOST_AUTO_TEST_CASE(fetch_through_injector_private) {
    asio::io_context ctx;

    TestDir root;

    const std::string injector_credentials = "username:password";

    using S = std::string;

    Injector injector(make_config<InjectorConfig>({
            S("./no_injector_exec"),
            S("--repo"), root.make_subdir("injector").string(),
            S("--credentials"), injector_credentials,
        }),
        ctx);

    Client client(ctx, make_config<ClientConfig>({
            S("./no_client_exec"),
            S("--repo"), root.make_subdir("client").string(),
            S("--injector-credentials"), injector_credentials,
            S("--cache-type=bep5-http"),
            S("--cache-http-public-key"), injector.cache_http_public_key(),
            S("--injector-tls-cert-file"), injector.tls_cert_file().string(),
        }));

    client.start();

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        while (true) {
            auto dht = client.get_dht();

            if (dht && dht->is_bootstrapped()) {
                break;
            }

            asio::steady_timer timer(ctx);
            timer.expires_after(100ms);
            timer.async_wait(yield);
        }

        injector.stop();
        client.stop();
    },
    [] (std::exception_ptr e) {
        if (e) std::rethrow_exception(e);
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

