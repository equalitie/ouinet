// #pragma once
#include "client.h"
#define BOOST_TEST_MODULE utility

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <async_sleep.h>
#include <chrono>
#include <climits>
#include <functional>
#include <iostream>
#include <namespaces.h>
#include <random>
#include <vector>

#include <util/async_generator.h>
#include <util/wait_condition.h>

#include "service.h"

using namespace std;
using namespace chrono;
namespace test = boost::unit_test;

using namespace ouinet;
namespace fs = ouinet::fs ;
namespace util = ouinet::util;
using ouiservice::i2poui::Service;
using ouiservice::i2poui::Client;

const std::string hello_message = "hello";

struct Setup {
    string testname;
    string testsuite;
    fs::path tempdir;

    Setup()
        : testname(test::framework::current_test_case().p_name)
        , testsuite(test::framework::get<test::test_suite>(test::framework::current_test_case().p_parent_id).p_name)
        , tempdir(fs::temp_directory_path() / "ouinet-cpp-tests" / testsuite / testname / fs::unique_path())
    {
        fs::create_directories(tempdir);
    }

    ~Setup() {
        fs::remove_all(tempdir);
    }
};

template<class Rep, class Period>
float as_seconds(std::chrono::duration<Rep, Period> duration) {
    return duration_cast<milliseconds>(duration).count() / 1000.f;
}

std::vector<unsigned char> generate_random_bytes(size_t size) {
    using random_bytes_engine = std::independent_bits_engine<
    std::default_random_engine, CHAR_BIT, unsigned char>;

    random_bytes_engine rbe;
    std::vector<unsigned char> data(size);
    std::generate(begin(data), end(data), std::ref(rbe));
    return data;
}

std::string byte_units(uint64_t count) {
    const uint64_t mb = 1024 * 1024;
    const uint64_t kb = 1024;

    if (count >= 1024 * 1024) {
        auto mbs = count / mb;
        auto rest = float((count - (mbs*mb))) / mb;
        return util::str(mbs, ".", int(rest*1000), "MiB");
    } else if (count >= kb) {
        auto kbs = count / kb;
        auto rest = float((count - (kbs*kb))) / kb;
        return util::str(kbs, ".", int(rest*1000), "KiB");
    } else {
        return util::str(count, "B");
    }
}
