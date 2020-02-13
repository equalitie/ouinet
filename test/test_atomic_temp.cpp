#define BOOST_TEST_MODULE atomic_temp
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>

#include <boost/filesystem.hpp>

#include <defer.h>
#include <util/atomic_dir.h>
#include <util/file_io.h>
#include <util/temp_dir.h>

#include <namespaces.h>


BOOST_AUTO_TEST_SUITE(ouinet_atomic_temp)

using namespace std;
using namespace ouinet;

static
void
populate_directory( const fs::path& dir
                  , const asio::executor& ex, sys::error_code& ec) {
    util::file_io::open_or_create(ex, dir / "testfile", ec);
    if (!ec) fs::create_directory(dir / "testdir", ec);
    if (!ec) util::file_io::open_or_create(ex, dir / "testdir" / "testfile", ec);
}

static
void
check_directory(const fs::path& dir) {
    BOOST_REQUIRE(fs::exists(dir));
    BOOST_REQUIRE(fs::is_directory(dir));
    BOOST_CHECK(fs::exists(dir / "testfile"));
    BOOST_CHECK(fs::is_regular_file(dir / "testfile"));
    BOOST_REQUIRE(fs::exists(dir / "testdir"));
    BOOST_REQUIRE(fs::is_directory(dir / "testdir"));
    BOOST_CHECK(fs::exists(dir / "testdir" / "testfile"));
    BOOST_CHECK(fs::is_regular_file(dir / "testdir" / "testfile"));
}

static const bool true_false[] = {true, false};

BOOST_DATA_TEST_CASE(test_temp_dir, boost::unit_test::data::make(true_false), keep) {
    asio::io_context ctx;

    fs::path td_path;
    auto remove_td = defer([&] {
        if (fs::exists(td_path)) fs::remove_all(td_path);
    });
    {
        sys::error_code ec;

        auto td = util::temp_dir::make(ec);
        BOOST_REQUIRE_EQUAL(ec.message(), "Success");

        BOOST_CHECK(td->keep_on_close());
        td->keep_on_close(keep);
        BOOST_CHECK_EQUAL(td->keep_on_close(), keep);

        td_path = td->path();
        BOOST_REQUIRE(fs::exists(td_path));
        BOOST_REQUIRE(fs::is_directory(td_path));

        populate_directory(td_path, ctx.get_executor(), ec);
        BOOST_REQUIRE_EQUAL(ec.message(), "Success");
    }

    if (!keep) {
        BOOST_CHECK(!fs::exists(td_path));
        return;
    }

    check_directory(td_path);
}

BOOST_DATA_TEST_CASE(test_atomic_dir, boost::unit_test::data::make(true_false), commit) {
    asio::io_context ctx;

    fs::path ad_temp_path, ad_path = fs::unique_path();
    auto remove_td = defer([&] {
        if (fs::exists(ad_path)) fs::remove_all(ad_path);
        if (fs::exists(ad_temp_path)) fs::remove_all(ad_temp_path);
    });
    {
        sys::error_code ec;

        auto ad = util::atomic_dir::make(ad_path, ec);
        BOOST_REQUIRE_EQUAL(ec.message(), "Success");

        BOOST_CHECK_EQUAL(ad->path(), ad_path);
        BOOST_REQUIRE(!fs::exists(ad_path));

        ad_temp_path = ad->temp_path();
        BOOST_CHECK(ad_temp_path != ad_path);
        BOOST_REQUIRE(fs::exists(ad_temp_path));
        BOOST_REQUIRE(fs::is_directory(ad_temp_path));

        populate_directory(ad_temp_path, ctx.get_executor(), ec);
        BOOST_REQUIRE_EQUAL(ec.message(), "Success");

        if (commit) {
            ad->commit(ec);
            BOOST_REQUIRE_EQUAL(ec.message(), "Success");
        }
    }

    if (!commit) {
        BOOST_CHECK(!fs::exists(ad_path));
        BOOST_CHECK(!fs::exists(ad_temp_path));
        return;
    }

    check_directory(ad_path);
}

BOOST_AUTO_TEST_SUITE_END()
