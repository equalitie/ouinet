#define BOOST_TEST_MODULE test_random_pages
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "namespaces.h"
#include "client.h"
#include "injector.h"
#include "util/file_io.h"
#include "util/random_url.h"
#include "util/request_builder.h"
#include "util/test_constants.h"
#include "util/test_dir.h"
#include "util/unwrap.h"

using namespace std;
using namespace ouinet;

namespace test_constants = test::constants::ceno;

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

template<class Config>
static Config make_config(const std::vector<std::string>& args) {
    static constexpr auto c_str = [](const std::string& str) {
        return str.c_str();
    };

    std::vector<const char*> argv;
    std::transform(args.begin(), args.end(), std::back_inserter(argv), c_str);
    return Config(argv.size(), argv.data());
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

BOOST_AUTO_TEST_CASE(
    test_fetch_random_page_from_wikipedia,
    * boost::unit_test::timeout(240)
) {
    asio::io_context ctx;
    run(ctx, [&ctx] (Async yield) {
        TestDir root;
        auto repo_dir = root.make_subdir("client").string();

        auto tls_cert_path = repo_dir + "/tls-inj-cert.pem";
        auto tls_cert_file = util::file_io::open_or_create(
            ctx.get_executor(), tls_cert_path);
        auto r = util::file_io::write(
            tls_cert_file.value(),
            asio::const_buffer(
                test_constants::tls_injector_cert.data(),
                test_constants::tls_injector_cert.size()),
            yield
        );
        BOOST_CHECK(r);

        Client client(ctx, make_config<ClientConfig>({
            "./no_client_exec"s,
            "--log-level=DEBUG"s,
            "--repo"s, repo_dir,
            "--injector-credentials"s, test_constants::injector_credentials,
            "--cache-type=bep5-http"s,
            "--cache-http-public-key"s, test_constants::cache_http_public_key,
            "--injector-tls-cert-file"s, tls_cert_path,
            "--disable-origin-access"s,
            // Bind to random ports to avoid clashes
            "--listen-on-tcp=127.0.0.1:0"s,
            "--front-end-ep=127.0.0.1:0"s,
            "--trace-root=client",
        }));
        client.start();

        auto rpi = Route::PublicInjector{CacheType::Bep5Http{}};
        auto url = random_url_from_wikipedia(yield);
        auto rq = CacheRequestBuilder(url.value()).set_route(rpi).build();
        auto rs = fetch_through_client(client, rq, yield);

        BOOST_CHECK_EQUAL(rs.result(), http::status::ok);
        BOOST_CHECK_EQUAL(rs[http_::response_source_hdr], http_::response_source_hdr_injector);

        client.stop();
    });
}
