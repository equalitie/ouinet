#define BOOST_TEST_MODULE test_fetch
#include <boost/test/unit_test.hpp>

#include <boost/asio/ssl.hpp>
#include "util/dht.h"
#include "util/test_dir.h"
#include "util/http_server.h"
#include "injector.h"
#include "client.h"
#include "util/random.h"
#include "ssl/util.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::bittorrent;
using namespace std::chrono_literals;
using namespace boost::asio::ip;
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

Request build_cache_request(const util::Url& url) {
    int version = 11;
    std::string host = url.host;
    if (!url.port.empty()) host += ":" + url.port;
    std::string target = url.reassemble();

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_group_hdr, target);
    return req;
}

Request build_origin_request(const util::Url& url) {
    int version = 11;
    std::string host = url.host;
    if (!url.port.empty()) host += ":" + url.port;
    std::string target = url.path;

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    return req;
}

Request build_private_request(const util::Url& url) {
    int version = 11;
    std::string host = url.host;
    if (!url.port.empty()) host += ":" + url.port;
    std::string target = url.reassemble();

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_private_hdr, "true");
    req.prepare_payload();
    return req;
}

Response fetch_through_client(const Client& client, Request req, Async yield) {
    boost::beast::tcp_stream stream(client.get_executor());
    stream.async_connect(client.get_proxy_endpoint(), yield).value();

    http::async_write(stream, req, yield).value();

    beast::flat_buffer b;
    Response res;
    http::async_read(stream, b, res, yield).value();
    return res;
}

asio::ssl::stream<boost::beast::tcp_stream> setup_tls_stream(tcp::socket socket, asio::ssl::context& ctx, std::string host) {
    asio::ssl::stream<boost::beast::tcp_stream> stream(std::move(socket), ctx);
    if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        sys::error_code ec;
        ec.assign(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
        static boost::source_location loc = BOOST_CURRENT_LOCATION;
        sys::throw_exception_from_error(ec, loc);
    }
    stream.set_verify_callback(asio::ssl::host_name_verification(host));
    return stream;
}

Response fetch_from_origin(util::Url url, asio::ssl::context& ctx, Async yield) {
    if (url.port.empty()) url.port = "443";
    if (url.path.empty()) url.path = "/";

    auto exec = yield.get_executor();

    tcp::resolver resolver(exec);
    auto const results = resolver.async_resolve(url.host, url.port, yield).value();

    auto req = build_origin_request(url);
    std::string host = req[http::field::host];

    tcp::socket socket(exec);
    asio::async_connect(socket, results, yield).value();

    auto stream = setup_tls_stream(std::move(socket), ctx, url.host);
    stream.async_handshake(asio::ssl::stream_base::client, yield).value();

    http::async_write(stream, req, yield).value();

    beast::flat_buffer b;
    Response res;
    http::async_read(stream, b, res, yield).value();

    (void) stream.async_shutdown(yield);

    BOOST_REQUIRE_EQUAL(res.result(), http::status::ok);

    return res;
}

Response fetch_from_origin(util::Url url, Async yield) {
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ouinet::ssl::util::load_tls_ca_certificates(ctx);
    ctx.set_verify_mode(asio::ssl::verify_peer);

    return fetch_from_origin(std::move(url), ctx, yield);
}

void check_exception(std::exception_ptr e) {
    try {
        if (e) {
            std::rethrow_exception(e);
        }
    } catch (const std::exception& e) {
        BOOST_FAIL("Test failed with exception: " << e.what());
    } catch (...) {
        BOOST_FAIL("Test failed with unknown exception");
    }
}

template<class F>
requires std::invocable<F, Async>
void run(asio::io_context& ctx, F&& async_test) {
    using namespace std::chrono;

    std::optional<steady_clock::time_point> spawn_end;

    asio::spawn(
        ctx,
        [&spawn_end, async_test = std::move(async_test)] (asio::yield_context yield) mutable {
            async_test(Async(yield));
            spawn_end = steady_clock::now();
        },
        check_exception
    );

    ctx.run();

    // Test that after the test ended, the `ctx.run()` function exited in a timely manner.
    // If `!spawn_end` then the test threw an exception which already makes the test fail.
    if (spawn_end) {
        auto test_end = steady_clock::now();
        auto elapsed_ms = duration_cast<milliseconds>(test_end - *spawn_end).count();
        // TODO: Keep reducing the allowed timeout
        BOOST_REQUIRE_LT(elapsed_ms, 5000);
    }
}

asio::ssl::context client_ssl_context_for(const HttpServer& server) {
    asio::ssl::context ctx{asio::ssl::context::tls_client};

    ctx.load_verify_file(server.certificate_path().string());
    ctx.set_verify_mode(asio::ssl::verify_peer);

    return ctx;
}

std::string generate_random_body() {
    // TODO: Some tests fail with larger body sizes
    //size_t min_size = 64;
    //size_t max_size = 2 * 1024 * 1024;
    //auto size = util::random::number<size_t>(min_size, max_size);
    size_t size = 65536;
    return util::random::printable_ascii(size);
}

BOOST_AUTO_TEST_CASE(server) {
    asio::io_context ctx;
    run(ctx, [] (Async yield) {
        TestDir root;
        auto server = HttpServer(yield.get_executor(), root.path());

        std::string body = generate_random_body();
        server.add_resource("/", body);

        auto ssl_ctx = client_ssl_context_for(server);

        auto url = util::Url::from(util::str("https://", server.authority())).value();
        auto rs = fetch_from_origin(url, ssl_ctx, yield);

        BOOST_CHECK_EQUAL(rs.body(), body);
    });
}

BOOST_AUTO_TEST_CASE(test_client_fetch_from_origin) {
    asio::io_context ctx;

    TestDir root;

    const std::string injector_credentials = "username:password";

    HttpServer server(ctx.get_executor(), root.make_subdir("server").path());

    Client client(ctx, make_config<ClientConfig>({
            "./no_client_exec"s,
            "--log-level=DEBUG"s,
            "--repo"s, root.make_subdir("client").string(),
            "--cache-type=none"s,
            // Bind to random ports to avoid clashes
            "--listen-on-tcp=127.0.0.1:0"s,
            "--front-end-ep=127.0.0.1:0"s,
            "--tls-ca-cert-store-file="s + server.certificate_path().string(),
            "--bt-bootstrap-no-default"
        }),
        util::LogPath("client"));

    // Clients are started explicitly
    client.start();

    run(ctx, [&, server = std::move(server)] (Async yield) mutable {
        auto body = generate_random_body();
        server.add_resource("/", body);

        auto url = util::Url::from(util::str("https://", server.authority(), "/")).value();

        auto rq = build_cache_request(url);

        // The "seeder" fetches the signed content through the "injector"
        auto rs1 = fetch_through_client(client, rq, yield);

        BOOST_REQUIRE_EQUAL(rs1.result(), http::status::ok);
        BOOST_REQUIRE_EQUAL(rs1[http_::response_source_hdr], http_::response_source_hdr_origin);
        BOOST_REQUIRE(rs1.body() == body);

        client.stop();
    });
}

// An integration test with three identities: the 'injector', a 'seeder' client
// and a 'leecher' client.
//
// * The 'seeder' client fetches a resource through the injector and stores it locally.
// * The 'leecher' client then fetches the resource from the 'seeder'.
//
// The test is using `MockDht` because the `MainlineDht` wouldn't work locally.
BOOST_AUTO_TEST_CASE(test_storing_into_and_fetching_from_the_cache) {
    get_logger().set_threshold(DEBUG);

    asio::io_context ctx;

    TestDir root;

    HttpServer server(ctx.get_executor(), root.make_subdir("server").path());
    server.add_resource("/", generate_random_body());
    auto url = util::Url::from(util::str("https://", server.authority(), "/")).value();

    run(ctx, [&, server = std::move(server)] (Async yield) {
        auto dht_nodes = spawn_dht_nodes(2, yield);
        auto dht_endpoint = *dht_nodes[0]->local_endpoints().begin();

        const std::string injector_credentials = "username:password";

    	Injector injector(
    	    make_config<InjectorConfig>({
                "./no_injector_exec"s,
                "--log-level=DEBUG",
                "--repo"s, root.make_subdir("injector").string(),
                "--credentials"s, injector_credentials,
                "--tls-ca-cert-store-file="s + server.certificate_path().string(),
                "--allow-private-targets",
                "--bt-bootstrap-no-default",
                "--bt-bootstrap-extra", util::str(dht_endpoint),
                "--bt-allow-martians"
            }),
            ctx,
            util::LogPath("injector")
        );

        Client seeder(
            ctx,
            make_config<ClientConfig>({
                "./no_client_exec"s,
                "--log-level=DEBUG"s,
                "--repo"s, root.make_subdir("seeder").string(),
                "--injector-credentials"s, injector_credentials,
                "--cache-type=bep5-http"s,
                "--cache-http-public-key"s, injector.cache_http_public_key(),
                "--injector-tls-cert-file"s, injector.tls_cert_file().string(),
                "--disable-origin-access"s,
                // Bind to random ports to avoid clashes
                "--listen-on-tcp=127.0.0.1:0"s,
                "--front-end-ep=127.0.0.1:0"s,
                "--allow-private-targets",
                "--bt-bootstrap-no-default",
                "--bt-bootstrap-extra", util::str(dht_endpoint),
                "--bt-allow-martians"
            }),
            util::LogPath("seeder")
        );

        Client leecher(
            ctx,
            make_config<ClientConfig>({
                "./no_client_exec"s,
                "--log-level=DEBUG"s,
                "--repo"s, root.make_subdir("leecher").string(),
                "--injector-credentials"s, injector_credentials,
                "--cache-type=bep5-http"s,
                "--cache-http-public-key"s, injector.cache_http_public_key(),
                "--injector-tls-cert-file"s, injector.tls_cert_file().string(),
                "--disable-origin-access"s,
                "--disable-injector-access"s,
                // Bind to random ports to avoid clashes
                "--listen-on-tcp=127.0.0.1:0"s,
                "--front-end-ep=127.0.0.1:0"s,
                "--allow-private-targets",
                "--bt-bootstrap-no-default",
                "--bt-bootstrap-extra", util::str(dht_endpoint),
                "--bt-allow-martians"
            }),
            util::LogPath("leecher")
        );

        // Clients are started explicitly
        seeder.start();
        leecher.start();

        auto ssl_ctx = client_ssl_context_for(server);
        auto control_body = fetch_from_origin(url, ssl_ctx, yield).body();

        auto rq = build_cache_request(url);

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
    });
}

// Test fetching without the Ouinet client involved. That is, start the injector
// and fetch through it a resource from an origin. Do it sequentially 30 times.
// TODO: Connect to injector using uTP/TLS
BOOST_AUTO_TEST_CASE(test_direct_to_injector_connect_proxy) {
    asio::io_context ctx;

    TestDir root;

    HttpServer server(ctx.get_executor(), root.make_subdir("server").path());
    server.add_resource("/", generate_random_body());
    auto url = util::Url::from(util::str("https://", server.authority(), "/")).value();

    tcp::endpoint injector_ep{
        asio::ip::address_v4::loopback(),
        4567
    };

    Injector injector(make_config<InjectorConfig>({
            "./no_injector_exec"s,
            "--repo"s, root.make_subdir("injector").string(),
            // TODO: Listen on a random port
            "--listen-on-tcp"s, util::str(injector_ep),
            "--tls-ca-cert-store-file="s + server.certificate_path().string(),
            "--allow-private-targets",
            "--bt-bootstrap-no-default"
        }),
        ctx,
        util::LogPath("injector"));

    run(ctx, [&, server = std::move(server)] (Async yield) {
        auto ssl_ctx = client_ssl_context_for(server);
        auto control_body = fetch_from_origin(url, ssl_ctx, yield).body();

        auto rq = build_private_request(url);

        for (uint16_t i = 0; i < 30; ++i) {
            // Connect to injector and establish HTTP CONNECT tunnel
            tcp::socket socket(yield.get_executor());

            socket.async_connect(injector_ep, yield);

            auto connect_rq = Request{
                http::verb::connect
                , url.host + ":" + (url.port.empty() ? "443" : url.port)
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
            auto stream = setup_tls_stream(std::move(socket), ssl_ctx, url.host);
            stream.async_handshake(asio::ssl::stream_base::client, yield);

            // Send and receive through the secure tunnel
            http::async_write(stream, rq, yield);

            Response rs;
            http::async_read(stream, buf, rs, yield);

            BOOST_REQUIRE_EQUAL(rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(rs.body(), control_body);
        }

        injector.stop();
    });
}

BOOST_AUTO_TEST_CASE(test_fetching_private_route_30_times) {
    get_logger().set_threshold(DEBUG);

    asio::io_context ctx;

    TestDir root;

    HttpServer server(ctx.get_executor(), root.make_subdir("server").path());
    server.add_resource("/", generate_random_body());
    auto url = util::Url::from(util::str("https://", server.authority(), "/")).value();

    run(ctx, [&, server = std::move(server)] (Async yield) {
        auto dhts = spawn_dht_nodes(2, yield);
        auto dht_ep = *dhts[0]->local_endpoints().begin();

        const std::string injector_credentials = "username:password";

    	Injector injector(
	        make_config<InjectorConfig>({
                "./no_injector_exec"s,
                "--repo"s, root.make_subdir("injector").string(),
                "--credentials"s, injector_credentials,
                "--allow-private-targets",
                "--bt-bootstrap-no-default",
                "--bt-bootstrap-extra", util::str(dht_ep),
                "--bt-allow-martians"
            }),
            ctx,
            util::LogPath("injector")
        );

        Client client(
            ctx,
            make_config<ClientConfig>({
                "./no_client_exec"s,
                "--log-level=DEBUG"s,
                "--repo"s, root.make_subdir("client").string(),
                "--injector-credentials"s, injector_credentials,
                "--cache-type=bep5-http"s,
                "--cache-http-public-key"s, injector.cache_http_public_key(),
                "--injector-tls-cert-file"s, injector.tls_cert_file().string(),
                "--disable-origin-access"s,
                // Bind to random ports to avoid clashes
                "--listen-on-tcp=127.0.0.1:0"s,
                "--front-end-ep=127.0.0.1:0"s,
                "--tls-ca-cert-store-file="s + server.certificate_path().string(),
                "--allow-private-targets",
                "--bt-bootstrap-no-default",
                "--bt-bootstrap-extra", util::str(dht_ep),
                "--bt-allow-martians"
            }),
            util::LogPath("client")
        );


        // Clients are started explicitly
        client.start();

        auto ssl_ctx = client_ssl_context_for(server);
        auto control_body = fetch_from_origin(url, ssl_ctx, yield).body();

        auto rq = build_private_request(url);

        for (uint16_t i = 0; i < 30; ++i) {
            auto rs = fetch_through_client(client, rq, yield);

            BOOST_REQUIRE_EQUAL(rs.result(), http::status::ok);
            BOOST_REQUIRE_EQUAL(rs[http_::response_source_hdr], http_::response_source_hdr_proxy);
            BOOST_REQUIRE_EQUAL(rs.body(), control_body);
        }

        injector.stop();
        client.stop();
    });
}
