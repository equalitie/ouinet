#define BOOST_TEST_MODULE test_i2p_fetch
#include <boost/test/unit_test.hpp>

#include <boost/beast/core.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <chrono>
#include "util/request_builder.h"
#include "util/http_client.h"
#include "util/http_server.h"
#include "util/test_dir.h"
#include "util/unwrap.h"
#include "util/i2p.h"
#include "bittorrent/mock_dht.h"
#include "injector.h"
#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/tracker.h"
#include "util/random.h"
#include "client.h"
#include "ssl/util.h"
#include "async_sleep.h"
#include "route.h"

using namespace std;
using namespace ouinet;
using namespace std::chrono_literals;
using namespace boost::asio::ip;
using bittorrent::MockDht;
using tcp = asio::ip::tcp;

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
using Response = http::response<http::string_body>;

Request build_cache_request(util::Url url, Route route, std::string resource_group) {
    return CacheRequestBuilder(url)
        .set_resource_group(resource_group)
        .set_route(route).build();
}

Response fetch_through_client(const Client& client, Request req, Async yield) {
    boost::beast::tcp_stream stream(client.get_executor());

    unwrap(stream.async_connect(client.get_proxy_endpoint(), yield));
    unwrap(http::async_write(stream, req, yield));

    beast::flat_buffer b;
    Response res;

    unwrap(http::async_read(stream, b, res, yield));

    return res;
}

void check_exception(std::exception_ptr e) {
    try {
        if (e) {
            std::rethrow_exception(e);
        }
    } catch (const std::exception& e) {
        BOOST_FAIL("Test failed with exception: " << e.what());
    }
}

template<class F>
void run(asio::io_context& ctx, F&& async_test) {
    using namespace std::chrono;

    std::optional<steady_clock::time_point> spawn_end;

    asio::spawn(ctx, [&spawn_end, async_test = std::move(async_test)] (asio::yield_context yield) {
            async_test(Async(yield));
            spawn_end = steady_clock::now();
        },
        check_exception);

    ctx.run();

    // Test that after the test ended, the `ctx.run()` function exited in a timely manner.
    // If `!spawn_end` then the test threw an exception which already makes the test fail.
    if (spawn_end) {
        auto elapsed = steady_clock::now() - *spawn_end;
        // TODO: Keep reducing the allowed timeout
        BOOST_REQUIRE_LT(elapsed, 5s);
    }
}

void wait_for_peer_on_tracker(
        I2pAddress::B32 tracker_addr,
        bittorrent::NodeID infohash,
        I2pAddress::B32 peer_addr,
        asio::ip::tcp::endpoint sam_ep,
        Async yield) {
    auto session = std::make_shared<I2pSession>(unwrap(I2pSession::create(sam_ep, yield)));
    auto tracker = I2pTrackerClient(session, tracker_addr);
    for (int i = 0; i < 120; ++i) {
        auto peers = unwrap(tracker.get_peers(infohash, yield));
        if (peers.contains(peer_addr)) {
            return;
        }
        async_sleep(1s, yield);
    }
    BOOST_FAIL("Failed to wait for peer appearing on the tracker");
}

struct TestCase {
    std::optional<HttpServer> server;
    std::vector<util::Url> urls;
};

// An integration test with three identities: the 'injector', a 'seeder' client
// and a 'leecher' client.
//
// * The 'seeder' client fetches a resource through the injector and stores it locally.
// * The 'leecher' client then fetches the resource from the 'seeder'.
//
// The test is using `MockDht` because the `MainlineDht` wouldn't work locally.
void test_storing_into_and_fetching_from_the_cache_case(asio::io_context& ctx, const TestDir& root, TestCase test_case) {
    // Logging is normally first enabled in either the Client or the Injector, but we want to
    // see log lines even before that (mainly from the I2P code).
    get_logger().set_threshold(DEBUG);

    const std::string injector_credentials = "username:password";
    auto tracker_addr = unwrap(I2pAddress::B32::parse("z2tfkf4t23gig3nfybnat2qarjl2f7dctcj63khfluqt2fdoikpa.b32.i2p"));
    const std::string i2p_fast_tunnel_hop_count = "1";

    auto swarms = std::make_shared<MockDht::Swarms>();

    run(ctx, [&, test_case = std::move(test_case)] (Async yield) {
        auto i2p_service = create_i2p_service(yield);
        auto sam_endpoint = unwrap(i2p_service.await_running_state(yield)).sam_endpoint;

        BOOST_TEST_MESSAGE("Setting up injector");

        auto injector_config = std::vector<std::string> {
            "./no_injector_exec"s,
            "--repo"s, root.make_subdir("injector").string(),
            "--credentials"s, injector_credentials,
            "--listen-on-i2p=true"s,
            "--enable-i2p-service-ext"s, util::str(sam_endpoint),
        };

        if (test_case.server) {
            injector_config.push_back("--tls-ca-cert-store-file="s + test_case.server->certificate_path().string());
            injector_config.push_back("--allow-private-targets");
        }

        Injector injector(make_config<InjectorConfig>(std::move(injector_config)),
            ctx,
            util::LogPath("injector"),
            std::make_shared<MockDht>("injector", ctx.get_executor(), swarms));

        BOOST_TEST_MESSAGE("Setting up seeder");

        auto seeder_config = std::vector<std::string> {
            "./no_client_exec"s,
            "--log-level=DEBUG"s,
            "--repo"s, root.make_subdir("seeder").string(),
            "--injector-credentials"s, injector_credentials,
            "--cache-type=bep3-http-over-i2p"s,
            "--cache-http-public-key"s, injector.cache_http_public_key(),
            "--injector-ep=i2p:" + unwrap(injector.i2p_address(yield)).to_b32().as_str(),
            "--i2p-bep3-tracker"s, tracker_addr.as_str(),
            "--injector-tls-cert-file"s, injector.tls_cert_file().string(),
            "--disable-origin-access"s,
            "--disable-proxy-access"s,
            "--i2p-hops-per-tunnel"s, i2p_fast_tunnel_hop_count,
            "--enable-i2p-service-ext"s, util::str(sam_endpoint),
            // XXX Bind to random ports to avoid clashes
            "--listen-on-tcp=127.0.0.1:0"s,
            "--front-end-ep=127.0.0.1:0"s,
        };

        if (test_case.server) {
            seeder_config.push_back("--allow-private-targets");
        }

        Client seeder(ctx, make_config<ClientConfig>(std::move(seeder_config)),
            util::LogPath("seeder"),
            [&ctx, swarms] () {
                auto dht = std::make_shared<MockDht>("seeder", ctx.get_executor(), swarms);
                dht->can_not_see("injector");
                return dht;
            });

        BOOST_TEST_MESSAGE("Setting up leecher");

        auto leecher_config = std::vector<std::string> {
            "./no_client_exec"s,
            "--log-level=DEBUG"s,
            "--repo"s, root.make_subdir("leecher").string(),
            "--cache-type=bep3-http-over-i2p"s,
            "--i2p-bep3-tracker"s, tracker_addr.as_str(),
            "--enable-i2p-service-ext"s, util::str(sam_endpoint),
            "--cache-http-public-key"s, injector.cache_http_public_key(),
            "--injector-tls-cert-file"s, injector.tls_cert_file().string(),
            "--disable-origin-access"s,
            "--disable-proxy-access"s,
            // Bind to random ports to avoid clashes
            "--listen-on-tcp=127.0.0.1:0"s,
            "--front-end-ep=127.0.0.1:0"s,
        };

        if (test_case.server) {
            leecher_config.push_back("--allow-private-targets");
        }

        Client leecher(ctx, make_config<ClientConfig>(std::move(leecher_config)),
            util::LogPath("leecher"),
            [&ctx, swarms] () {
                auto dht = std::make_shared<MockDht>("leecher", ctx.get_executor(), swarms);
                dht->can_not_see("seeder");
                dht->can_not_see("injector");
                return dht;
            });

        BOOST_TEST_MESSAGE("Starting clients");

        // Clients are started explicitly
        seeder.start();
        leecher.start();

        auto resource_group = util::random::from_set(20, "0123456789abcdefghijklmnoprstuvxyz");

        BOOST_TEST_MESSAGE("Requesting control bodys from origin");

        std::vector<std::string> control_body;

        for (auto& url : test_case.urls) {
            BOOST_TEST_MESSAGE("    " << url);

            if (test_case.server) {
                asio::ssl::context ssl_ctx = test_case.server->ssl_context_for_client();
                control_body.push_back(unwrap(fetch_from_origin(url, ssl_ctx, yield)).body());
            }
            else {
                control_body.push_back(unwrap(fetch_from_origin(url, yield)).body());
            }
        }

        BOOST_TEST_MESSAGE("Seeder fetching through injector");
        for (size_t i = 0; i < test_case.urls.size(); ++i) {
            auto& url = test_case.urls[i];

            BOOST_TEST_MESSAGE("    " << url);

            auto rs = fetch_through_client(
                    seeder,
                    build_cache_request(url, Route::PublicInjector{CacheType::Bep3HTTPOverI2P{}}, resource_group),
                    yield);

            BOOST_REQUIRE_EQUAL(rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(rs[http_::response_source_hdr], http_::response_source_hdr_injector);
            BOOST_REQUIRE_EQUAL(rs.body(), control_body[i]);
        }

        BOOST_TEST_MESSAGE("Waiting for seeder to announce on BEP3/I2P tracker");
        // Wait for seeder to announce
        wait_for_peer_on_tracker(
                tracker_addr,
                leecher.compute_infohash_for_resource_group(resource_group),
                unwrap(seeder.local_i2p_address(yield)).to_b32(),
                sam_endpoint,
                yield);

        BOOST_TEST_MESSAGE("Leecher fetching from seeder");
        for (size_t i = 0; i < test_case.urls.size(); ++i) {
            auto& url = test_case.urls[i];

            BOOST_TEST_MESSAGE("    " << url);
            // The "leecher" client fetches the signed content from the "seeder"
            auto rs = fetch_through_client(
                    leecher,
                    build_cache_request(url, Route::DCache{CacheType::Bep3HTTPOverI2P{}}, resource_group),
                    yield);

            BOOST_REQUIRE_EQUAL(rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(rs[http_::response_source_hdr], http_::response_source_hdr_dist_cache);
            BOOST_REQUIRE_EQUAL(rs.body(), control_body[i]);
        }

        BOOST_TEST_MESSAGE("Stopping nodes");
        injector.stop();
        seeder.stop();
        leecher.stop();
    });
}

BOOST_AUTO_TEST_CASE(test_storing_into_and_fetching_from_the_cache__gitlab) {
    asio::io_context ctx;

    TestDir root;

    const util::Url url = util::Url::from("https://gitlab.com/ceno-app/ceno-android/-/raw/main/LICENSE").value();

    test_storing_into_and_fetching_from_the_cache_case(ctx, root, TestCase { std::nullopt, {url} });
}

BOOST_AUTO_TEST_CASE(test_storing_into_and_fetching_from_the_cache__local) {
    asio::io_context ctx;

    TestDir root;

    HttpServer server(ctx.get_executor(), root.make_subdir("server").path());
    server.add_resource("/", util::random::printable_ascii(512));
    auto url = util::Url::from(util::str("https://", server.authority(), "/")).value();

    test_storing_into_and_fetching_from_the_cache_case(ctx, root, TestCase { std::move(server), {url} });
}

BOOST_AUTO_TEST_CASE(test_storing_into_and_fetching_from_the_cache__local_many) {
    asio::io_context ctx;

    TestDir root;

    HttpServer server(ctx.get_executor(), root.make_subdir("server").path());

    std::vector<util::Url> urls;

    for (size_t i = 0; i < 32; ++i) {
        auto path = util::str("/r", i);
        server.add_resource(path, util::random::printable_ascii(512));
        urls.push_back(unwrap(util::Url::from(util::str("https://", server.authority(), path))));
    }

    test_storing_into_and_fetching_from_the_cache_case(ctx, root, TestCase { std::move(server), std::move(urls) });
}

