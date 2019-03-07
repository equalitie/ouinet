#define BOOST_TEST_MODULE persistent_lru_cache
#include <boost/test/included/unit_test.hpp>

#include <util/persistent_lru_cache.h>
#include <defer.h>
#include <namespaces.h>
#include <iostream>
#include <util/file_io.h>

BOOST_AUTO_TEST_SUITE(persistent_lru_cache)

using namespace std;
using namespace ouinet;
using namespace ouinet::util;
using File = asio::posix::stream_descriptor;

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

    void write(File& f, Cancel& cancel, asio::yield_context yield) const
    {
        sys::error_code ec;

        file_io::write_number<uint64_t>(f, size(), cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        file_io::write(f, asio::buffer(*this), cancel, yield[ec]);
    }

    void read(File& f, Cancel& cancel, asio::yield_context yield)
    {
        sys::error_code ec;

        auto s = file_io::read_number<uint64_t>(f, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        resize(s);
        file_io::read(f, asio::buffer(*this), cancel, yield[ec]);
    }
};

using Lru = PersistentLruCache<StringEntry>;

BOOST_AUTO_TEST_CASE(test_initialize)
{
    asio::io_service ios;
    Cancel cancel;

    auto dir = fs::temp_directory_path()
             / fs::unique_path("ouinet-persistent-lru-cache-test-%%%%-%%%%");

    auto on_exit = defer([&] { fs::remove_all(dir); });

    BOOST_REQUIRE(!exists(dir));

    // Sometimes it's useful to comment out the above requirement and just
    // delete the existing one. Note that it'll also be deleted once the
    // io_service is done (at the bottom of this functions).
    if (exists(dir)) {
        fs::remove_all(dir);
    }

    cerr << "LRU cache test dir: " << dir << endl;

    const unsigned max_cache_size = 2;

    asio::spawn(ios, [&] (auto yield) {
        sys::error_code ec;

        {
            auto lru = Lru::load(ios, dir, max_cache_size, cancel, yield[ec]);

            BOOST_REQUIRE(!ec);

            lru->insert("hello1", "world1", cancel, yield[ec]);
            BOOST_REQUIRE(!ec);

            BOOST_REQUIRE(lru->find("not-there") == lru->end());

            {
                auto i = lru->find("hello1");
                BOOST_REQUIRE(i != lru->end());
            }

            lru->insert("hello2", "world2", cancel, yield[ec]);
            BOOST_REQUIRE(!ec);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            lru->insert("hello3", "world3", cancel, yield[ec]);
            BOOST_REQUIRE(!ec);

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

            auto lru = Lru::load(ios, dir, max_cache_size, cancel, yield[ec]);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);
            BOOST_REQUIRE_EQUAL(lru->size(), count_files_in_dir(dir));
        }

        // Reload again into a smaller cache
        {
            const unsigned new_max_cache_size = max_cache_size - 1;

            BOOST_REQUIRE(new_max_cache_size < max_cache_size);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            auto lru = Lru::load(ios, dir, new_max_cache_size, cancel, yield[ec]);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), new_max_cache_size);
            BOOST_REQUIRE_EQUAL(lru->size(), count_files_in_dir(dir));
        }
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
