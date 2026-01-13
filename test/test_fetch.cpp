#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/beast/core.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/stacktrace.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/system/result.hpp>
#include <namespaces.h>
#include <iostream>
#include <chrono>
#include "util/test_dir.h"
#include "util/crypto.h"
#include "bittorrent/mock_dht.h"
#include "injector.h"
#include "client.h"
#include "util/str.h"

BOOST_AUTO_TEST_SUITE(ouinet_cpp_integration_tests)

using namespace std;
using namespace ouinet;
using namespace std::chrono_literals;
using namespace boost::asio::ip;
using bittorrent::MockDht;
namespace ssl = asio::ssl;
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

const util::Url test_url = util::Url::from("https://gitlab.com/ceno-app/ceno-android/-/raw/main/LICENSE").value();

Request build_cache_request() {
    int version = 11;
    std::string host = test_url.host;
    std::string target = test_url.reassemble();

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_group_hdr, target);
    return req;
}

Request build_origin_request() {
    int version = 11;
    std::string host = test_url.host;
    std::string target = test_url.path;

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    return req;
}

Request build_private_request() {
    int version = 11;
    std::string host = test_url.host;
    std::string target = test_url.reassemble();

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_private_hdr, "true");
    req.prepare_payload();
    return req;
}

Response fetch_through_client(const Client& client, Request req, asio::yield_context yield) {
    boost::beast::tcp_stream stream(client.get_executor());
    stream.async_connect(client.get_proxy_endpoint(), yield);

    http::async_write(stream, req, yield);

    beast::flat_buffer b;
    Response res;
    http::async_read(stream, b, res, yield);
    return res;
}

ssl::stream<boost::beast::tcp_stream> setup_tls_stream(tcp::socket socket, ssl::context& ctx, std::string host) {
    ssl::stream<boost::beast::tcp_stream> stream(std::move(socket), ctx);
    if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        sys::error_code ec;
        ec.assign(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
        static boost::source_location loc = BOOST_CURRENT_LOCATION;
        sys::throw_exception_from_error(ec, loc);
    }
    stream.set_verify_callback(ssl::host_name_verification(host));
    return stream;
}

Response fetch_from_origin(asio::yield_context yield) {
    auto url = test_url;

    if (url.port.empty()) url.port = "443";
    if (url.path.empty()) url.path = "/";

    auto exec = yield.get_executor();

    tcp::resolver resolver(exec);
    auto const results = resolver.async_resolve(url.host, url.port, yield);

    ssl::context ctx{ssl::context::tlsv12_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);

    auto req = build_origin_request();
    std::string host = req[http::field::host];

    tcp::socket socket(exec);
    asio::async_connect(socket, results, yield);

    auto stream = setup_tls_stream(std::move(socket), ctx, host);
    stream.async_handshake(ssl::stream_base::client, yield);

    http::async_write(stream, req, yield);

    beast::flat_buffer b;
    Response res;
    http::async_read(stream, b, res, yield);

    sys::error_code ignored_ec;
    stream.shutdown(ignored_ec);

    BOOST_REQUIRE_EQUAL(res.result(), http::status::ok);

    return res;
}

// An integration test with three identities: the 'injector', a 'seeder' client
// and a 'leecher' client.
//
// * The 'seeder' client fetches a resource through the injector and stores it locally.
// * The 'leecher' client then fetches the resource from the 'seeder'.
//
// The test is using `MockDht` because the `MainlineDht` wouldn't work locally.
BOOST_AUTO_TEST_CASE(test_storing_into_and_fetching_from_the_cache) {
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
        util::LogPath("injector"),
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
        util::LogPath("seeder"),
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
        util::LogPath("leecher"),
        [&ctx, swarms] () {
            auto dht = std::make_shared<MockDht>("leecher", ctx.get_executor(), swarms);
            dht->can_not_see("injector");
            return dht;
        });

    // Clients are started explicitly
    seeder.start();
    leecher.start();

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        auto control_body = fetch_from_origin(yield).body();

        auto rq = build_cache_request();

        // The "seeder" fetches the signed content through the "injector"
        auto rs1 = fetch_through_client(seeder, rq, yield);

        BOOST_CHECK_EQUAL(rs1.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs1[http_::response_source_hdr], http_::response_source_hdr_injector);
        BOOST_CHECK_EQUAL(rs1.body(), control_body);

        // The "leecher" client fetches the signed content from the "seeder"
        auto rs2 = fetch_through_client(leecher, rq, yield);

        BOOST_CHECK_EQUAL(rs2.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs2[http_::response_source_hdr], http_::response_source_hdr_dist_cache);
        BOOST_CHECK_EQUAL(rs2.body(), control_body);

        injector.stop();
        seeder.stop();
        leecher.stop();
    },
    [] (std::exception_ptr e) {
        if (e) std::rethrow_exception(e);
    });

    ctx.run();
}

// Test fetching without the Ouinet client involved. That is, start the injector
// and fetch through it a resource from an origin. Do it sequentially 30 times.
// TODO: Connect to injector using uTP/TLS
BOOST_AUTO_TEST_CASE(test_direct_to_injector_connect_proxy) {
    asio::io_context ctx;

    TestDir root;

    using S = std::string;

    auto swarms = std::make_shared<MockDht::Swarms>();

    tcp::endpoint injector_ep{
        asio::ip::address_v4::any(),
        4567
    };

    Injector injector(make_config<InjectorConfig>({
            S("./no_injector_exec"),
            S("--repo"), root.make_subdir("injector").string(),
            // TODO: Listen on a random port
            S("--listen-on-tcp"), util::str(injector_ep),
        }),
        ctx,
        util::LogPath("injector"),
        std::make_shared<MockDht>("injector", ctx.get_executor(), swarms));

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        auto control_body = fetch_from_origin(yield).body();

        auto rq = build_private_request();

        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        for (uint16_t i = 0; i < 30; ++i) {
            // Connect to injector and establish HTTP CONNECT tunnel
            tcp::socket socket(yield.get_executor());

            socket.async_connect(injector_ep, yield);

            auto connect_rq = Request{
                http::verb::connect
                , test_url.host + ":" + (test_url.port.empty() ? "443" : test_url.port)
                , 11 /* HTTP/1.1 */
            };
            connect_rq.set(http::field::host, connect_rq.target());

            http::async_write(socket, connect_rq, yield);

            Response connect_rs;

            beast::flat_buffer buf;
            http::async_read(socket, buf, connect_rs, yield);

            BOOST_REQUIRE_EQUAL(connect_rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(buf.size(), 0);

            // Do TLS handshake with the origin over the established tunnel
            auto stream = setup_tls_stream(std::move(socket), ctx, test_url.host);
            stream.async_handshake(ssl::stream_base::client, yield);

            // Send and receive through the secure tunnel
            http::async_write(stream, rq, yield);

            Response rs;
            http::async_read(stream, buf, rs, yield);

            BOOST_REQUIRE_EQUAL(rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(rs.body(), control_body);
        }

        injector.stop();
    },
    [] (std::exception_ptr e) {
        if (e) std::rethrow_exception(e);
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_fetching_private_route_30_times) {
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
        util::LogPath("injector"),
        std::make_shared<MockDht>("injector", ctx.get_executor(), swarms));

    Client client(ctx, make_config<ClientConfig>({
            S("./no_client_exec"),
            S("--log-level=DEBUG"),
            S("--repo"), root.make_subdir("client").string(),
            S("--injector-credentials"), injector_credentials,
            S("--cache-type=bep5-http"),
            S("--cache-http-public-key"), injector.cache_http_public_key(),
            S("--injector-tls-cert-file"), injector.tls_cert_file().string(),
            S("--disable-origin-access"),
            // Bind to random ports to avoid clashes
            S("--listen-on-tcp=127.0.0.1:0"),
            S("--front-end-ep=127.0.0.1:0"),
        }),
        util::LogPath("client"),
        [&ctx, swarms] () {
            return std::make_shared<MockDht>("client", ctx.get_executor(), swarms);
        });

    // Clients are started explicitly
    client.start();

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        auto control_body = fetch_from_origin(yield).body();

        auto rq = build_private_request();

        for (uint16_t i = 0; i < 30; ++i) {
            auto rs = fetch_through_client(client, rq, yield);

            BOOST_REQUIRE_EQUAL(rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(rs[http_::response_source_hdr], http_::response_source_hdr_proxy);
            BOOST_REQUIRE_EQUAL(rs.body(), control_body);
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

