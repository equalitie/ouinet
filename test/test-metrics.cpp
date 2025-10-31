#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <namespaces.h>
#include <iostream>
#include "task.h"
#include "async_sleep.h"
#include "cxx/metrics.h"
#include "util/test_dir.h"

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

BOOST_AUTO_TEST_CASE(enable_enable) {
    TestDir test_dir;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(test_dir.path(), move(encryption_key));

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
    TestDir test_dir;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(test_dir.path(), move(encryption_key));

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
    TestDir test_dir;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(test_dir.path(), move(encryption_key));

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
    TestDir test_dir;

    asio::io_context ctx;

    task::spawn_detached(ctx, [&] (asio::yield_context yield) {
        auto encryption_key = *metrics::EncryptionKey::validate(public_key_pem);
        auto client = metrics::Client(test_dir.path(), move(encryption_key));

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

