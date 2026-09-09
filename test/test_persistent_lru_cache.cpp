#define BOOST_TEST_MODULE persistent_lru_cache
#include <boost/test/unit_test.hpp>
#include <boost/asio/detached.hpp>

#include <util/persistent_lru_cache.h>
#include <defer.h>
#include <namespaces.h>
#include <iostream>
#include <util/file_io.h>
#include <task.h>
#include "util/unwrap.h"

namespace utf = boost::unit_test;

BOOST_AUTO_TEST_SUITE(persistent_lru_cache, * utf::timeout(2))

using namespace std;
using namespace ouinet;
using namespace ouinet::util;
using File = async_file_handle;

namespace fs = boost::filesystem;

unsigned count_files_in_dir(const fs::path& dir)
{
    unsigned ret = 0;
    for ([[maybe_unused]] auto& _ : fs::directory_iterator(dir)) {
        ++ret;
    }
    return ret;
}

struct StringEntry : public std::string {

    using std::string::string;

    [[nodiscard]]
    std::expected<void, sys::error_code>
    write(File& f, Async yield) const
    {
        if (auto r = file_io::write_number<uint64_t>(f, size(), yield); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = file_io::write(f, asio::buffer(*this), yield); !r) {
            return std::unexpected(r.error());
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    read(File& f, Async yield)
    {
        if (auto r = file_io::read_number<uint64_t>(f, yield); !r) {
            return std::unexpected(r.error());
        }
        else {
            resize(*r);
        }
        if (auto r = file_io::read(f, asio::buffer(*this), yield); !r) {
            return std::unexpected(r.error());
        }
        return {};
    }
};

using Lru = PersistentLruCache<StringEntry>;

static void run_spawned(std::function<void(Async)> f) {
    asio::io_context ctx;

    task::spawn_detached(ctx.get_executor(), [f = std::move(f)] (auto yield) {
            try {
                f(Async(yield));
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_initialize)
{
    auto dir = fs::temp_directory_path()
             / fs::unique_path("ouinet-persistent-lru-cache-test-%%%%-%%%%");

    auto on_exit = ouinet::defer([&] { fs::remove_all(dir); });

    BOOST_REQUIRE(!exists(dir));

    // Sometimes it's useful to comment out the above requirement and just
    // delete the existing one. Note that it'll also be deleted once the
    // io_context is done (at the bottom of this functions).
    if (exists(dir)) {
        fs::remove_all(dir);
    }

    cerr << "LRU cache test dir: " << dir << endl;

    const unsigned max_cache_size = 2;

    run_spawned([&] (Async yield) {
        {
            auto lru = unwrap(Lru::load(dir, max_cache_size, yield));

            unwrap(lru->insert("hello1", "world1", yield));

            BOOST_REQUIRE(lru->find("not-there") == lru->end());

            {
                auto i = lru->find("hello1");
                BOOST_REQUIRE(i != lru->end());
            }

            unwrap(lru->insert("hello2", "world2", yield));

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            unwrap(lru->insert("hello3", "world3", yield));

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            {
                // "hello1" should now be evicted
                auto i = lru->find("hello1");
                BOOST_REQUIRE(i == lru->end());
            }

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            {
                auto i = lru->find("hello2");
                BOOST_REQUIRE(i != lru->end());
                BOOST_REQUIRE(i.value() == "world2");
            }

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);
        }

        // Reload from disk
        {
            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            auto lru = unwrap(Lru::load(dir, max_cache_size, yield));

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);
            BOOST_REQUIRE_EQUAL(lru->size(), count_files_in_dir(dir));
        }

        // Reload again into a smaller cache
        {
            const unsigned new_max_cache_size = max_cache_size - 1;

            BOOST_REQUIRE(new_max_cache_size < max_cache_size);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            auto lru = unwrap(Lru::load(dir, new_max_cache_size, yield));

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), new_max_cache_size);
            BOOST_REQUIRE_EQUAL(lru->size(), count_files_in_dir(dir));
        }
    });
}

struct DataEntry {
    const std::string* data = nullptr;  // only set and used until writing

    [[nodiscard]]
    std::expected<void, sys::error_code>
    write(File& f, Async yield)
    {
        if (auto r = file_io::write(f, asio::buffer(*data), yield); !r) {
            return std::unexpected(r.error());
        }
        data = nullptr;
        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    read(File&, Async) {
        return {};
    }
};

using DataLru = PersistentLruCache<DataEntry>;

BOOST_AUTO_TEST_CASE(test_open_value)
{
    auto dir = fs::temp_directory_path()
             / fs::unique_path("ouinet-persistent-lru-cache-test-%%%%-%%%%");

    auto on_exit = ouinet::defer([&] { fs::remove_all(dir); });

    BOOST_REQUIRE(!exists(dir));

    cerr << "LRU cache test dir: " << dir << endl;

    const unsigned max_cache_size = 1;
    const std::string key("test");
    const std::string data(4200, 'x');  // bigger than usual cache block

    run_spawned([&] (auto yield) {
        // Create cache and insert element
        {
            auto lru = unwrap(DataLru::load(dir, max_cache_size, yield));
            unwrap(lru->insert(key, DataEntry{&data}, yield));
        }

        // Reload cache and open element data
        {
            auto lru = unwrap(DataLru::load(dir, max_cache_size, yield));

            auto i = lru->find(key);
            BOOST_REQUIRE(i != lru->end());

            auto f = unwrap(i.open());

            std::string data_in(data.size(), '\0');
            unwrap(file_io::read(f, asio::buffer(data_in), yield));
            BOOST_REQUIRE_EQUAL(data_in, data);
        }

        // Update entry while another reader is accessing it
        {
            std::string data_in;

            auto lru = unwrap(DataLru::load(dir, max_cache_size, yield));

            auto i = lru->find(key);
            BOOST_REQUIRE(i != lru->end());

            auto f_old = unwrap(i.open());

            const std::string data_new(data.size(), 'y');
            unwrap(lru->insert(key, DataEntry({&data_new}), yield));

            auto f_new = unwrap(i.open());

            // This should yield the new data
            data_in.resize(data_new.size(), '\0');
            unwrap(file_io::read(f_new, asio::buffer(data_in), yield));
            BOOST_REQUIRE_EQUAL(data_in, data_new);

            // This should yield the old data, not the new one
            data_in.resize(data.size(), '\0');
            unwrap(file_io::read(f_old, asio::buffer(data_in), yield));
            BOOST_REQUIRE_EQUAL(data_in, data);
        }
    });
}

BOOST_AUTO_TEST_SUITE_END()
