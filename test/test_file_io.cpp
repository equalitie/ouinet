#define BOOST_TEST_MODULE Tests for file_io module

#ifdef __MINGW32__
#include <winsock2.h>
#endif // __MINGW32__

#include <boost/test/included/unit_test.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include "util/signal.h"
#include "util/file_io.h"
#include "../test/util/base_fixture.hpp"
#include "task.h"

#ifndef _WIN32
const int INVALID_HANDLE_VALUE = -1;
#endif

namespace asio = boost::asio;
namespace sys = boost::system;
namespace ut = boost::unit_test;
namespace file_io = ouinet::util::file_io;
namespace task = ouinet::task;

using Cancel = ouinet::Signal<void()>;

struct fixture_file_io:fixture_base
{
    asio::io_context ctx;
    sys::error_code ec;
    size_t default_timer = 2;
    Cancel cancel;

};

BOOST_FIXTURE_TEST_SUITE(suite_file_io, fixture_file_io);

BOOST_AUTO_TEST_CASE(test_open_or_create)
{
    temp_file temp_file{test_id};
    task::spawn_detached(ctx, [&](asio::yield_context yield){
            asio::steady_timer timer{ctx};
            timer.expires_after(std::chrono::seconds(default_timer));
            timer.async_wait(yield);
            auto aio_file = file_io::open_or_create(
                    ctx.get_executor(),
                    temp_file.get_name(),
                    ec);
    });
    ctx.run();
    BOOST_TEST(boost::filesystem::exists(temp_file.get_name()));
}

BOOST_AUTO_TEST_CASE(test_cursor_operations, * ut::depends_on("suite_file_io/test_open_or_create"))
{
    std::string expected_string = "0123456789";
    size_t expected_position = expected_string.size();
    temp_file temp_file{test_id};

    if (std::ofstream output{temp_file.get_name()} ) {
        output << expected_string;
    }
    BOOST_REQUIRE(boost::filesystem::exists(temp_file.get_name()));
    if (std::ifstream input{temp_file.get_name()} ) {
        std::string current_string;
        input >> current_string;
        BOOST_REQUIRE(expected_string == current_string);
    }

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        asio::steady_timer timer{ctx};
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
        async_file_handle aio_file = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);

        // Test file size
        size_t expected_size = expected_string.size();
        size_t actual_size = file_io::file_size(aio_file, ec);
        BOOST_TEST(expected_size == actual_size);

        // Test cursor movement
        expected_position = 7;
        file_io::fseek(aio_file, expected_position, ec);
        BOOST_TEST(expected_position == file_io::current_position(aio_file, ec));

        // Test remaining size
        BOOST_TEST(3 == file_io::file_remaining_size(aio_file, ec));

        // Read remaining chars
        std::string data_in(3, '\0');
        file_io::read(aio_file, asio::buffer(data_in), cancel, yield);
        BOOST_TEST("789" == data_in);
    });
    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_async_write)
{
    temp_file temp_file{test_id};
    std::string expected_string = "one-two-three";

    // Create the file and write at the end of it a few times
    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        async_file_handle aio_file = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        file_io::write(aio_file, boost::asio::const_buffer("one", 3), cancel, yield);
        file_io::write(aio_file, boost::asio::const_buffer("-two", 4), cancel, yield);
        file_io::write(aio_file, boost::asio::const_buffer("-three", 6), cancel, yield);
        asio::steady_timer timer{ctx};
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
    });
    ctx.run();

    BOOST_REQUIRE(boost::filesystem::exists(temp_file.get_name()));
    if (std::ifstream input{temp_file.get_name()} ) {
        std::string current_string;
        input >> current_string;
        BOOST_TEST(expected_string == current_string);
    }
}

// Check for Asio's iocp issue https://github.com/chriskohlhoff/asio/issues/1346
// for which we made a patch
BOOST_AUTO_TEST_CASE(test_multi_buffer)
{
    temp_file temp_file{test_id};

    task::spawn_detached(ctx, [&] (auto yield) {
        sys::error_code ec;

        std::string bw0 = "01";
        std::string bw1 = "23456";

        // Write to a file
        {
            auto f = file_io::open_or_create(ctx.get_executor(), temp_file.get_name(), ec);
            BOOST_TEST_REQUIRE(!ec, "Open file for writing: " << ec.message());

            std::vector<asio::const_buffer> bs = { asio::buffer(bw0), asio::buffer(bw1) };

            asio::async_write(f, bs, yield[ec]);
            BOOST_TEST_REQUIRE(!ec, "Writing to file: " << ec.message());
        }

        // Read from the file and check
        {
            auto f = file_io::open_readonly(ctx.get_executor(), temp_file.get_name(), ec);
            BOOST_TEST_REQUIRE(!ec, "Open file for reading: " << ec.message());

            std::string br0 = "XXX";
            std::string br1 = "XXXX";
            std::vector<asio::mutable_buffer> bs = { asio::buffer(br0), asio::buffer(br1) };

            asio::async_read(f, bs, yield[ec]);
            BOOST_TEST_REQUIRE(!ec, "Reading from file: " << ec.message());

            BOOST_REQUIRE_EQUAL(bw0 + bw1, br0 + br1);
        }
    });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_read_only_operations)
{
    temp_file temp_file{test_id};
    std::string expected_string("ABC123xyz");
    std::string data_in(expected_string.size(), '\0');

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        asio::steady_timer timer{ctx};

        // Create test file and close it
        async_file_handle aio_file_rw = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        file_io::write(aio_file_rw, boost::asio::const_buffer("ABC123xyz", 9), cancel, yield);
        aio_file_rw.close();

        // Open the file again in read-only mode
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
        async_file_handle aio_file_ro = file_io::open_readonly(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);

        file_io::read(aio_file_ro, asio::buffer(data_in), cancel, yield);
        BOOST_TEST(expected_string == data_in);
        aio_file_ro.close();

        // Check that the file is opened in read-only mode
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
        aio_file_ro = file_io::open_readonly(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        BOOST_CHECK(!ec);
        file_io::write(aio_file_ro, boost::asio::const_buffer("DEF456uvw", 9), cancel, yield[ec]);
#ifndef _WIN32
        BOOST_CHECK(ec.value() == 9); // Expected errno 9, Bad file descriptor
#endif
        ec.clear();
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
        aio_file_ro = file_io::open_readonly(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        file_io::read(aio_file_ro, asio::buffer(data_in), cancel, yield[ec]);
        BOOST_CHECK(!ec);
        BOOST_TEST(expected_string == data_in); // Checking with expected_string as the file should be unmodified
        aio_file_ro.close();
    });
    ctx.run();
}


BOOST_AUTO_TEST_CASE(
    test_dup_fd
#ifdef _WIN32
    , * ut::disabled() * ut::description("file_io::dup_fd not implemented yet for Windows")
#endif
){
    temp_file temp_file{test_id};
    std::string expected_string = "abcXYZ";

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        async_file_handle aio_file = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);

        native_handle_t aio_handler_dup = file_io::dup_fd(aio_file, ec);
        BOOST_REQUIRE(aio_handler_dup != INVALID_HANDLE_VALUE);
        // TODO: Do something with the duplicated handler

        file_io::write(aio_file, boost::asio::const_buffer("abcXYZ", 6), cancel, yield);
        asio::steady_timer timer{ctx};
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
    });
    ctx.run();

    BOOST_REQUIRE(boost::filesystem::exists(temp_file.get_name()));
    if (std::ifstream input{temp_file.get_name()} ) {
        std::string current_string;
        input >> current_string;
        BOOST_TEST(expected_string == current_string);
    }
}

BOOST_AUTO_TEST_CASE(test_truncate_file)
{
    temp_file temp_file{test_id};
    std::string expected_string = "abc";

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        async_file_handle aio_file = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        file_io::write(aio_file, boost::asio::const_buffer("xyz", 3), cancel, yield);
        asio::steady_timer timer{ctx};
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);

        file_io::truncate(aio_file, 0, ec);
        file_io::write(aio_file, boost::asio::const_buffer("abc", 3), cancel, yield);
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
    });
    ctx.run();

    BOOST_REQUIRE(boost::filesystem::exists(temp_file.get_name()));
    if (std::ifstream input{temp_file.get_name()} ) {
        std::string current_string;
        input >> current_string;
        BOOST_TEST(expected_string == current_string);
    }
}

BOOST_AUTO_TEST_CASE(test_check_or_create_directory)
{
    temp_file temp_file{test_id};

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        bool success = file_io::check_or_create_directory(
                temp_file.get_name(), ec);
        BOOST_CHECK(success);
        asio::steady_timer timer{ctx};
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
    });
    ctx.run();
    BOOST_REQUIRE(boost::filesystem::exists(temp_file.get_name()));
    BOOST_CHECK(boost::filesystem::is_directory(temp_file.get_name()));
}

BOOST_AUTO_TEST_CASE(test_remove_file)
{
    temp_file temp_file{test_id};
    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        async_file_handle aio_file = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        BOOST_CHECK(boost::filesystem::exists(temp_file.get_name()));
        file_io::remove_file(temp_file.get_name());
        BOOST_CHECK(!boost::filesystem::is_directory(temp_file.get_name()));
    });
    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_read_and_write_numbers)
{
    temp_file temp_file{test_id};
    size_t expected_number = 1248;

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        async_file_handle aio_file = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        file_io::write_number<size_t>(aio_file, expected_number, cancel, yield[ec]);
        BOOST_REQUIRE(!ec);
        asio::steady_timer timer{ctx};
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
        file_io::fseek(aio_file, 0, ec);
        auto actual_number = file_io::read_number<size_t>(aio_file, cancel, yield[ec]);
        BOOST_REQUIRE(!ec);
        BOOST_CHECK(expected_number == actual_number);
    });
    ctx.run();
}

std::string shrink(std::string str){
    auto replace = [&str](const std::string& toReplace, const std::string& replacement){
        size_t pos = str.find(toReplace);
        while (pos != std::string::npos) {
            str.replace(pos, toReplace.length(), replacement);
            pos = str.find(toReplace, pos + replacement.length());
        }
    };

    replace(std::string(16, 'x'), ".");
    replace(std::string(16, '.'), "o");
    replace(std::string(16, 'o'), "O");

    replace(std::string(16, 'y'), ",");
    replace(std::string(16, ','), "i");
    replace(std::string(16, 'i'), "I");
    return str;
}

BOOST_AUTO_TEST_CASE(test_read_files)
{
    temp_file temp_file{test_id};

    // auto fill_count = 31 * 1024 + 1019;
    auto fill_count = 63 * 1024 + 1019;
    auto fill_char_1 = 'x';
    auto fill_char_2 = 'y';
    auto fill_1 = std::string(fill_count, fill_char_1);
    auto fill_2 = std::string(fill_count, fill_char_2);

    std::string expected{
            "aaaa" + fill_1 +
            "bbbb" + fill_2 +
            "cccc" };
    auto expected_size = expected.size();
    std::string data_in(expected_size, '\0');

    task::spawn_detached(ctx, [&](asio::yield_context yield) {
        asio::steady_timer timer{ctx};

        // Create test file and close it
        async_file_handle aio_file_rw = file_io::open_or_create(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);
        file_io::write(aio_file_rw, boost::asio::const_buffer(expected.data(), expected_size), cancel, yield);
        aio_file_rw.close();

        // Open the file again in read-only mode
        timer.expires_after(std::chrono::seconds(default_timer));
        timer.async_wait(yield);
        async_file_handle aio_file_ro = file_io::open_readonly(
                ctx.get_executor(),
                temp_file.get_name(),
                ec);

        file_io::read(aio_file_ro, asio::buffer(data_in), cancel, yield);
        //std::cout << shrink(expected) << std::endl;
        //std::cout << shrink(data_in) << std::endl;

        BOOST_TEST(shrink(expected) == shrink(data_in));
        aio_file_ro.close();

    });
    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END();
