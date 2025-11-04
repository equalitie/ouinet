#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/beast/core.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <iostream>
#include <chrono>
#include "util/test_dir.h"
#include "util/crypto.h"
#include "bittorrent/mock_dht.h"
#include "injector.h"
#include "client.h"

BOOST_AUTO_TEST_SUITE(ouinet_integration_tests)

using namespace std;
using namespace ouinet;
using namespace std::chrono_literals;
using namespace boost::asio::ip;
using bittorrent::MockDht;

template<class Config>
static Config make_config(const std::vector<std::string>& args) {
    static constexpr auto c_str = [](const std::string& str) {
        return str.c_str();
    };

    std::vector<const char*> argv;
    std::transform(args.begin(), args.end(), std::back_inserter(argv), c_str);
    return Config(argv.size(), argv.data());
}

using Request = http::request<http::string_body>;
using Response = http::response<http::dynamic_body>;

Response fetch(const Client& client, asio::yield_context yield) {
    boost::beast::tcp_stream stream(client.get_executor());
    stream.async_connect(client.get_proxy_endpoint(), yield);

    int version = 11;
    std::string host = "gitlab.com";
    std::string target = "https://" + host + "/ceno-app/ceno-android/-/raw/main/LICENSE";

    http::request<http::string_body> req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_group_hdr, target);

    http::async_write(stream, req, yield);

    beast::flat_buffer b;
    http::response<http::dynamic_body> res;

    http::async_read(stream, b, res, yield);

    stream.socket().shutdown(tcp::socket::shutdown_both);

    return res;
}

// WIP: Currently just creates an injector and a client and waits for them to bootstrap
BOOST_AUTO_TEST_CASE(fetch_through_injector_public) {
    asio::io_context ctx;

    TestDir root;

    const std::string injector_credentials = "username:password";

    using S = std::string;

    auto swarms = std::make_shared<MockDht::Swarms>();

    Injector injector(make_config<InjectorConfig>({
            S("./no_injector_exec"),
            S("--repo"), root.make_subdir("injector").string(),
            S("--credentials"), injector_credentials,
        }),
        ctx,
        std::make_shared<MockDht>("injector", ctx.get_executor(), swarms));

    Client seeder(ctx, make_config<ClientConfig>({
            S("./no_client_exec"),
            S("--log-level=DEBUG"),
            S("--repo"), root.make_subdir("seeder").string(),
            S("--injector-credentials"), injector_credentials,
            S("--cache-type=bep5-http"),
            S("--cache-http-public-key"), injector.cache_http_public_key(),
            S("--injector-tls-cert-file"), injector.tls_cert_file().string(),
            S("--disable-origin-access"),
            // Bind to random ports to avoid clashes
            S("--listen-on-tcp=127.0.0.1:0"),
            S("--front-end-ep=127.0.0.1:0"),
        }),
        [&ctx, swarms] () {
            return std::make_shared<MockDht>("seeder", ctx.get_executor(), swarms);
        });

    Client leecher(ctx, make_config<ClientConfig>({
            S("./no_client_exec"),
            S("--log-level=DEBUG"),
            S("--repo"), root.make_subdir("leecher").string(),
            S("--injector-credentials"), injector_credentials,
            S("--cache-type=bep5-http"),
            S("--cache-http-public-key"), injector.cache_http_public_key(),
            S("--injector-tls-cert-file"), injector.tls_cert_file().string(),
            S("--disable-origin-access"),
            // Bind to random ports to avoid clashes
            S("--listen-on-tcp=127.0.0.1:0"),
            S("--front-end-ep=127.0.0.1:0"),
        }),
        [&ctx, swarms] () {
            auto dht = std::make_shared<MockDht>("leecher", ctx.get_executor(), swarms);
            dht->can_not_see("injector");
            return dht;
        });

    // Clients are started explicitly
    seeder.start();
    leecher.start();

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        // The "seeder" fetches the signed content through the "injector"
        auto rs1 = fetch(seeder, yield);

        BOOST_CHECK_EQUAL(rs1.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs1[http_::response_source_hdr], http_::response_source_hdr_injector);

        // The "leecher" client fetches the signed content from the "seeder"
        auto rs2 = fetch(leecher, yield);

        BOOST_CHECK_EQUAL(rs2.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs2[http_::response_source_hdr], http_::response_source_hdr_dist_cache);

        injector.stop();
        seeder.stop();
        leecher.stop();
    },
    [] (std::exception_ptr e) {
        if (e) std::rethrow_exception(e);
    });

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

