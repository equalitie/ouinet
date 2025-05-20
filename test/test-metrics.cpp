#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>
#include <namespaces.h>
#include <iostream>
#include "task.h"
#include "async_sleep.h"
#include "cxx/metrics.h"

BOOST_AUTO_TEST_SUITE(ouinet_metrics)

using namespace std;
using namespace ouinet;
namespace fs = boost::filesystem;
namespace ut = boost::unit_test;

// Look into rust/record_format.md for information on how to generate this.
string public_key_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VuAyEAdrkFffyZjr5r6k1Jl2+27fv0KvJu+H8Xk7GwjKnRiHc=\n"
    "-----END PUBLIC KEY-----";

struct Setup {
    string testname;
    string testsuite;
    fs::path tempdir;

    Setup()
        : testname(ut::framework::current_test_case().p_name)
        , testsuite(ut::framework::get<ut::test_suite>(ut::framework::current_test_case().p_parent_id).p_name)
        , tempdir(fs::temp_directory_path() / "ouinet-cpp-tests" / testsuite / testname / fs::unique_path())
    {
        fs::create_directories(tempdir);
    }

    ~Setup() {
        fs::remove_all(tempdir);
    }
};

BOOST_AUTO_TEST_CASE(enable_enable) {
    Setup setup;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(setup.tempdir, move(encryption_key));

        Cancel cancel;

        async_sleep(ctx, 300ms, cancel, yield);
        client.enable(ctx.get_executor(), [](auto, auto, auto) {});

        async_sleep(ctx, 300ms, cancel, yield);
        client.enable(ctx.get_executor(), [](auto, auto, auto) {});
    });

    ctx.run();

    // Disable warnings about not checking assertions, they are used in the Rust code.
    BOOST_REQUIRE(true);
}

BOOST_AUTO_TEST_CASE(disable_disable) {
    Setup setup;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(setup.tempdir, move(encryption_key));

        Cancel cancel;

        async_sleep(ctx, 300ms, cancel, yield);
        client.disable();

        async_sleep(ctx, 300ms, cancel, yield);
        client.disable();
    });

    ctx.run();

    // Disable warnings about not checking assertions, they are used in the Rust code.
    BOOST_REQUIRE(true);
}

BOOST_AUTO_TEST_CASE(enable_disable_enable) {
    Setup setup;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(setup.tempdir, move(encryption_key));

        Cancel cancel;

        async_sleep(ctx, 300ms, cancel, yield);
        client.enable(ctx.get_executor(), [](auto, auto, auto) {});

        async_sleep(ctx, 300ms, cancel, yield);
        client.disable();

        async_sleep(ctx, 300ms, cancel, yield);
        client.enable(ctx.get_executor(), [](auto, auto, auto) {});
    });

    ctx.run();

    // Disable warnings about not checking assertions, they are used in the Rust code.
    BOOST_REQUIRE(true);
}

BOOST_AUTO_TEST_CASE(disable_enable_disable) {
    Setup setup;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(setup.tempdir, move(encryption_key));

        Cancel cancel;

        async_sleep(ctx, 300ms, cancel, yield);
        client.disable();

        async_sleep(ctx, 300ms, cancel, yield);
        client.enable(ctx.get_executor(), [](auto, auto, auto) {});

        async_sleep(ctx, 300ms, cancel, yield);
        client.disable();
    });

    ctx.run();

    // Disable warnings about not checking assertions, they are used in the Rust code.
    BOOST_REQUIRE(true);
}

BOOST_AUTO_TEST_SUITE_END()

