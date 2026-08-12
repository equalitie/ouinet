#pragma once

#include <boost/test/unit_test.hpp>
#include <boost/filesystem/path.hpp>

#include <chrono>

#include "ouiservice/i2p/sam.h"
#include "ouiservice/i2p/service.h"
#include "async_sleep.h"
#include "util/async.h"
#include "util/select.h"
#include "namespaces.h"

namespace ouinet {

I2pService create_i2p_service(Async yield) {
#if defined(_WIN32) || defined(MINGW)
    const char* i2pd_exe = "i2pd.exe";
#else
    const char* i2pd_exe = "i2pd";
#endif
    auto test_path = fs::path(boost::unit_test::framework::master_test_suite().argv[0]);
    auto test_dir = test_path.parent_path();
    auto build_dir = test_dir.parent_path();
    auto i2pd_exe_path = build_dir / i2pd_exe;
    auto i2pd_data_dir = TestDir::for_global_fixture();

    BOOST_TEST_MESSAGE("The `i2pd` executable path is set to " << i2pd_exe_path);
    BOOST_TEST_MESSAGE("The `i2pd` data directory is set to  " << i2pd_data_dir.path());

    auto i2p_service_config = I2pService::Config {
        I2pService::ConfigExternal {
            Sam::default_endpoint()
        },
        I2pService::ConfigI2pdExe{
            i2pd_exe_path,
            i2pd_data_dir.path() // datadir
        },
        I2pService::ConfigI2pdLib {
            i2pd_data_dir.path() // datadir
        }
    };

    using namespace std::chrono;
    using namespace std::chrono_literals;

    auto start = steady_clock::now();

    auto i2p_service = I2pService::start(i2p_service_config, yield.get_executor(), {}, {});

    auto max_wait = 5min;
    BOOST_TEST_MESSAGE("Waiting up to " << max_wait << " for I2P service to start");

    auto r = timeout(max_wait, [&](Async yield) { return i2p_service.await_running_state(yield); }, yield);

    if (!r) {
        BOOST_FAIL("I2P service did not start within " << max_wait);
    }
    else if (!*r) {
        BOOST_FAIL("I2P service failed to start");
    }

    BOOST_TEST_MESSAGE("I2P service started in " << duration_cast<milliseconds>(steady_clock::now() - start));

    return i2p_service;
}

} // namespace
