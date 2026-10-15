#define BOOST_TEST_MODULE Tests for file_io module

#ifdef __MINGW32__
#include <winsock2.h>
#endif // __MINGW32__

#include <boost/test/unit_test.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include "util/file_io.h"
#include "../test/util/base_fixture.hpp"
#include "task.h"
#include "util/unwrap.h"
#include "util/async.h"

#ifndef _WIN32
const int INVALID_HANDLE_VALUE = -1;
#endif

namespace asio = boost::asio;
namespace sys = boost::system;
namespace ut = boost::unit_test;
namespace file_io = ouinet::util::file_io;
namespace task = ouinet::task;
using ouinet::Async;
using ouinet::unwrap;

struct fixture_file_io:fixture_base
{
    asio::io_context ctx;
    asio::any_io_executor exec;

    fixture_file_io() : exec(ctx.get_executor()) {}

    template<class Job>
    void run(Job job) {
        task::spawn_detached(ctx.get_executor(), [job = std::move(job)] (asio::yield_context yield) {
                try {
                    job(Async(yield));
                }
                catch (const std::exception& e) {
                    BOOST_ERROR(std::string("Test ended with exception: ") + e.what());
                }
            });

        ctx.run();
    }
};

BOOST_FIXTURE_TEST_SUITE(suite_file_io, fixture_file_io);

BOOST_AUTO_TEST_CASE(test_open_or_create)
{
    temp_file temp_file{test_id};
    auto aio_file = unwrap(file_io::open_or_create(exec, temp_file.get_name()));
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

    run([&](Async yield) {
        async_file_handle aio_file = unwrap(file_io::open_or_create(exec, temp_file.get_name()));

        // Test file size
        size_t expected_size = expected_string.size();
        size_t actual_size = unwrap(file_io::file_size(aio_file));
        BOOST_TEST(expected_size == actual_size);

        // Test cursor movement
        expected_position = 7;
        unwrap(file_io::fseek(aio_file, expected_position));
        BOOST_TEST(expected_position == unwrap(file_io::current_position(aio_file)));

        // Test remaining size
        BOOST_TEST(3 == unwrap(file_io::file_remaining_size(aio_file)));

        // Read remaining chars
        std::string data_in(3, '\0');
        unwrap(file_io::read(aio_file, asio::buffer(data_in), yield));
        BOOST_TEST("789" == data_in);
    });
}

BOOST_AUTO_TEST_CASE(test_async_write)
{
    temp_file temp_file{test_id};
    std::string expected_string = "one-two-three";

    // Create the file and write at the end of it a few times
    run([&](Async yield) {
        async_file_handle aio_file = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));

        unwrap(file_io::write(aio_file, boost::asio::const_buffer("one", 3), yield));
        unwrap(file_io::write(aio_file, boost::asio::const_buffer("-two", 4), yield));
        unwrap(file_io::write(aio_file, boost::asio::const_buffer("-three", 6), yield));
    });

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

    run([&] (Async yield) {
        std::string bw0 = "01";
        std::string bw1 = "23456";

        // Write to a file
        {
            auto f = unwrap(file_io::open_or_create(exec, temp_file.get_name()));

            std::vector<asio::const_buffer> bs = { asio::buffer(bw0), asio::buffer(bw1) };

            unwrap(asio::async_write(f, bs, yield));
        }

        // Read from the file and check
        {
            auto f = unwrap(file_io::open_readonly(exec, temp_file.get_name()));

            std::string br0 = "XXX";
            std::string br1 = "XXXX";
            std::vector<asio::mutable_buffer> bs = { asio::buffer(br0), asio::buffer(br1) };

            unwrap(asio::async_read(f, bs, yield));

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

    run([&](Async yield) {
        // Create test file and close it
        async_file_handle aio_file_rw = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));

        unwrap(file_io::write(aio_file_rw, boost::asio::const_buffer("ABC123xyz", 9), yield));
        aio_file_rw.close();

        // Open the file again in read-only mode
        async_file_handle aio_file_ro = unwrap(file_io::open_readonly(
                exec,
                temp_file.get_name()));

        unwrap(file_io::read(aio_file_ro, asio::buffer(data_in), yield));
        BOOST_TEST(expected_string == data_in);
        aio_file_ro.close();

        // Check that the file is opened in read-only mode
        aio_file_ro = unwrap(file_io::open_readonly(
                exec,
                temp_file.get_name()));
        auto r = file_io::write(aio_file_ro, boost::asio::const_buffer("DEF456uvw", 9), yield);
#ifndef _WIN32
        BOOST_CHECK(r.error().value() == 9); // Expected errno 9, Bad file descriptor
#endif
        aio_file_ro = unwrap(file_io::open_readonly(
                exec,
                temp_file.get_name()));
        unwrap(file_io::read(aio_file_ro, asio::buffer(data_in), yield));
        BOOST_TEST(expected_string == data_in); // Checking with expected_string as the file should be unmodified
        aio_file_ro.close();
    });
}


BOOST_AUTO_TEST_CASE(
    test_dup_fd
#ifdef _WIN32
    , * ut::disabled() * ut::description("file_io::dup_fd not implemented yet for Windows")
#endif
){
    temp_file temp_file{test_id};
    std::string expected_string = "abcXYZ";

    run([&](Async yield) {
        async_file_handle aio_file = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));

        native_handle_t aio_handler_dup = unwrap(file_io::dup_fd(aio_file));
        BOOST_REQUIRE(aio_handler_dup != INVALID_HANDLE_VALUE);
        // TODO: Do something with the duplicated handler

        unwrap(file_io::write(aio_file, boost::asio::const_buffer("abcXYZ", 6), yield));
    });

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

    run([&](Async yield) {
        async_file_handle aio_file = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));

        unwrap(file_io::write(aio_file, boost::asio::const_buffer("xyz", 3), yield));

        unwrap(file_io::truncate(aio_file, 0));
        unwrap(file_io::write(aio_file, boost::asio::const_buffer("abc", 3), yield));
    });

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

    bool success = unwrap(file_io::check_or_create_directory(temp_file.get_name()));
    BOOST_CHECK(success);
    BOOST_REQUIRE(boost::filesystem::exists(temp_file.get_name()));
    BOOST_CHECK(boost::filesystem::is_directory(temp_file.get_name()));
}

BOOST_AUTO_TEST_CASE(test_remove_file)
{
    temp_file temp_file{test_id};

    // Scope to auto close the file handle, otherwise Windows would complain
    // when removing the file.
    {
        async_file_handle aio_file = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));
        BOOST_CHECK(boost::filesystem::exists(temp_file.get_name()));
    }

    unwrap(file_io::remove_file(temp_file.get_name()));
    BOOST_CHECK(!boost::filesystem::is_directory(temp_file.get_name()));
}

BOOST_AUTO_TEST_CASE(test_read_and_write_numbers)
{
    temp_file temp_file{test_id};
    size_t expected_number = 1248;

    run([&](Async yield) {
        async_file_handle aio_file = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));
        unwrap(file_io::write_number<size_t>(aio_file, expected_number, yield));
        unwrap(file_io::fseek(aio_file, 0));
        auto actual_number = unwrap(file_io::read_number<size_t>(aio_file, yield));
        BOOST_CHECK(expected_number == actual_number);
    });
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

    run([&](Async yield) {
        // Create test file and close it
        async_file_handle aio_file_rw = unwrap(file_io::open_or_create(
                exec,
                temp_file.get_name()));
        unwrap(file_io::write(aio_file_rw, boost::asio::const_buffer(expected.data(), expected_size), yield));
        aio_file_rw.close();

        // Open the file again in read-only mode
        async_file_handle aio_file_ro = unwrap(file_io::open_readonly(
                exec,
                temp_file.get_name()));

        unwrap(file_io::read(aio_file_ro, asio::buffer(data_in), yield));
        //std::cout << shrink(expected) << std::endl;
        //std::cout << shrink(data_in) << std::endl;

        BOOST_TEST(shrink(expected) == shrink(data_in));
        aio_file_ro.close();
    });
}

BOOST_AUTO_TEST_SUITE_END();
