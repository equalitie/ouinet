#define BOOST_TEST_MODULE persistent_lru_cache
#include <boost/test/included/unit_test.hpp>

#include <util/persistent_lru_cache.h>
#include <defer.h>
#include <namespaces.h>
#include <iostream>

BOOST_AUTO_TEST_SUITE(persistent_lru_cache)

using namespace std;
using namespace ouinet;
using namespace ouinet::util;
namespace fs = boost::filesystem;

vector<uint8_t> to_vec(const char* c)
{
    auto uc = (const uint8_t*) c;
    return vector<uint8_t>(uc, uc + strlen(c));
}

unsigned count_files_in_dir(const fs::path& dir)
{
    unsigned ret = 0;
    for ([[maybe_unused]] auto& _ : fs::directory_iterator(dir)) {
        ++ret;
    }
    return ret;
}

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
            auto lru = PersistentLruCache::load( ios
                                               , dir
                                               , max_cache_size
                                               , cancel
                                               , yield[ec]);

            BOOST_REQUIRE(!ec);

            lru->insert("hello1", to_vec("world1"), cancel, yield[ec]);
            BOOST_REQUIRE(!ec);

            BOOST_REQUIRE(lru->find("not-there") == lru->end());

            {
                auto i = lru->find("hello1");
                BOOST_REQUIRE(i != lru->end());
            }

            lru->insert("hello2", to_vec("world2"), cancel, yield[ec]);
            BOOST_REQUIRE(!ec);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            lru->insert("hello3", to_vec("world3"), cancel, yield[ec]);
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

                auto data = i.value(cancel, yield[ec]);
                BOOST_REQUIRE(!ec);
                BOOST_REQUIRE(data == to_vec("world2"));
            }

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);
        }

        // Reload from disk
        {
            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            auto lru = PersistentLruCache::load( ios
                                               , dir
                                               , max_cache_size
                                               , cancel
                                               , yield[ec]);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);
            BOOST_REQUIRE_EQUAL(lru->size(), count_files_in_dir(dir));
        }

        // Reload again into a smaller cache
        {
            const unsigned new_max_cache_size = max_cache_size - 1;

            BOOST_REQUIRE(new_max_cache_size < max_cache_size);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), max_cache_size);

            auto lru = PersistentLruCache::load( ios
                                               , dir
                                               , new_max_cache_size
                                               , cancel
                                               , yield[ec]);

            BOOST_REQUIRE_EQUAL(count_files_in_dir(dir), new_max_cache_size);
            BOOST_REQUIRE_EQUAL(lru->size(), count_files_in_dir(dir));
        }
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
