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
#include <ouisync.hpp>
#include <ouisync/service.hpp>
#include "util/test_dir.h"
#include "util/crypto.h"
#include "bittorrent/mock_dht.h"
#include "injector.h"
#include "client.h"
#include "util/str.h"

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

Request build_cache_request(util::Url url, std::string group) {
    int version = 11;
    std::string host = url.host;
    std::string target = url.reassemble();

    Request req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http_::request_group_hdr, group);
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

Response fetch_from_origin(util::Url url, asio::yield_context yield) {
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

// The `injector` and `seeder` will create a "crawl" which will then be copied
// to `seeder`s ouisync repo for the `leecher` to retrive it.
BOOST_AUTO_TEST_CASE(test_fetching_from_ouisync) {
    asio::io_context ctx;

    TestDir root;

    const std::string injector_credentials = "username:password";

    auto group = "test_group";
    auto swarms = std::make_shared<MockDht::Swarms>();

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        Injector injector(make_config<InjectorConfig>({
                "./no_injector_exec"s,
                "--repo"s, root.make_subdir("injector").string(),
                "--credentials"s, injector_credentials,
            }),
            ctx,
            util::LogPath("injector"),
            std::make_shared<MockDht>("injector", ctx.get_executor(), swarms));

        auto seeder_dir = root.make_subdir("seeder");

        Client seeder(ctx, make_config<ClientConfig>({
                "./no_client_exec"s,
                "--log-level=DEBUG"s,
                "--repo"s, seeder_dir.string(),
                "--injector-credentials"s, injector_credentials,
                "--cache-type=bep5-http"s,
                "--cache-http-public-key"s, injector.cache_http_public_key(),
                "--injector-tls-cert-file"s, injector.tls_cert_file().string(),
                "--disable-origin-access"s,
                // Bind to random ports to avoid clashes
                "--listen-on-tcp=127.0.0.1:0"s,
                "--front-end-ep=127.0.0.1:0"s,
            }),
            util::LogPath("seeder"),
            [&ctx, swarms] () {
                return std::make_shared<MockDht>("seeder", ctx.get_executor(), swarms);
            });

        auto ouisync_service_dir = root.make_subdir("ouisync");
        ouisync::Service service(yield.get_executor());
        service.start(ouisync_service_dir.string().c_str(), "ouisync-service", yield);

        auto session = ouisync::Session::connect(ouisync_service_dir.path(), yield);

        session.bind_network({"quic/0.0.0.0:0"}, yield);
        session.set_store_dirs({ouisync_service_dir.make_subdir("store").string()}, yield);
        session.set_mount_root(ouisync_service_dir.make_subdir("mount").string(), yield);
        session.set_local_discovery_enabled(true, yield);

        auto page_index = session.create_repository("page_index", yield);
        page_index.mount(yield);
        page_index.set_sync_enabled(true, yield);

        Client leecher(ctx, make_config<ClientConfig>({
                "./no_client_exec"s,
                "--log-level=DEBUG"s,
                "--repo"s, root.make_subdir("leecher").string(),
                "--cache-type=ouisync"s,
                "--ouisync-page-index"s, page_index.share(ouisync::AccessMode::READ, yield).value,
                "--disable-origin-access"s,
                // Bind to random ports to avoid clashes
                "--listen-on-tcp=127.0.0.1:0"s,
                "--front-end-ep=127.0.0.1:0"s,
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

        auto control_body = fetch_from_origin(test_url, yield).body();

        auto rq = build_cache_request(test_url, group);

        // The "seeder" fetches the signed content through the "injector"
        auto rs1 = fetch_through_client(seeder, rq, yield);

        BOOST_CHECK_EQUAL(rs1.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs1[http_::response_source_hdr], http_::response_source_hdr_injector);
        BOOST_CHECK_EQUAL(rs1.body(), control_body);

        // Create a repo and copy the fetched content into it
        auto page_repo = session.create_repository(group, yield);
        page_repo.mount(yield);
        page_repo.set_sync_enabled(true, yield);

        fs::copy(
            seeder_dir.path() / "bep5_http",
            ouisync_service_dir.path() / "mount" / group,
            fs::copy_options::recursive |
            // Files in the source directory have '-rw------' permissions, but
            // Ouisync currently doesn't support changing the defaults which
            // are '-rw-rw-r--' and returns an `operation_not_supported` error
            // when attempted to change it.
            fs::copy_options::ignore_attribute_errors
        );

        // Create an entry in the `page_index` repo with the new repo
        auto page_token = page_repo.share(ouisync::AccessMode::READ, yield).value;
        auto file = page_index.create_file("/"s + group, yield);
        file.write(0, {page_token.begin(), page_token.end()}, yield);
        file.close(yield);

        // The "leecher" client fetches the content from the Ouisync `session`
        auto rs2 = fetch_through_client(leecher, rq, yield);

        BOOST_CHECK_EQUAL(rs2.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs2[http_::response_source_hdr], http_::response_source_hdr_ouisync);
        BOOST_CHECK_EQUAL(rs2.body(), control_body);

        injector.stop();
        seeder.stop();
        leecher.stop();
    },
    check_exception);

    ctx.run();
}

