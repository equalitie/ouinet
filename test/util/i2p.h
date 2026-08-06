#pragma once

#include <boost/test/unit_test.hpp>
#include <boost/filesystem/path.hpp>

#include <chrono>
#include <iostream>

#include "ouiservice/i2p/sam.h"
#include "ouiservice/i2p/i2pd.h"
#include "async_sleep.h"
#include "util/async.h"
#include "task.h"
#include "unwrap.h"
#include "namespaces.h"

namespace ouinet {

// Check if i2p is running outside of the test. If yes, `{}` is returned and
// tests will connect to SAM port on that service. If no, the `i2pd` standalone
// executable will be launched to which the tests will connect instead. The
// `i2pd` process is represented by the returned value and the process is
// killed witen the `I2pd` instance is destroyed.
std::optional<I2pd> ensure_i2p_service(Async yield) {
    using namespace std::chrono_literals;

    BOOST_TEST_MESSAGE("Testing if I2P is already running");

    const unsigned int os_try_count = 5;
    const unsigned int i2pd_try_count = 10;

    for (unsigned int i = 0; i < os_try_count; ++i) {
        auto sam = Sam::connect(Sam::default_endpoint(), yield);
        if (sam.has_value()) {
            BOOST_TEST_MESSAGE("I2P is running, using that one");
            return {};
        }
        BOOST_TEST_MESSAGE("No I2P service detected (" << (i+1) << "/" << os_try_count << ")");
        async_sleep(1s, yield);
    }

    auto test_path = fs::path(boost::unit_test::framework::master_test_suite().argv[0]);
    auto test_dir = test_path.parent_path();
    auto build_dir = test_dir.parent_path();
    auto i2pd_path = build_dir / "i2pd";
    auto i2pd_data_dir = TestDir::for_global_fixture();

    BOOST_TEST_MESSAGE("No I2P service found, starting \"" << i2pd_path << "\"");
    BOOST_TEST_MESSAGE("I2pd data dir: " << i2pd_data_dir.path() << "\n");

    auto i2pd = unwrap(I2pd::start_exe(i2pd_path, i2pd_data_dir.path(), yield.get_executor(), util::LogPath("i2pd")));

    BOOST_TEST_MESSAGE("Waiting for i2pd to get ready");

    for (unsigned int i = 0; i < i2pd_try_count; ++i) {
        auto sam = Sam::connect(Sam::default_endpoint(), yield);
        if (sam.has_value()) {
            BOOST_TEST_MESSAGE("I2pd looks ready");
            return i2pd;
        }
        BOOST_TEST_MESSAGE("I2pd is not ready yet (" << (i+1) << "/" << i2pd_try_count << ")");
        async_sleep(1s, yield);
    }

    BOOST_FAIL("I2pd did not get ready within a timeout");

    return {};
}

} // namespace
