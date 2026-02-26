#include "../test/util/i2p_utils.hpp"
#include "i2pd/libi2pd/api.h"

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

BOOST_AUTO_TEST_CASE(test_connect_and_exchange) {
    Setup setup;
    asio::io_context ctx;
    string datadir_arg = "--datadir="+setup.tempdir.string();
    std::vector<const char*> argv({"i2pouiservice", datadir_arg.data()});

    i2p::api::InitI2P(argv.size(), (char**) argv.data(), argv[0]);
    i2p::api::StopI2P();
    i2p::api::InitI2P(argv.size(), (char**) argv.data(), argv[0]);
    i2p::api::StopI2P();

}

BOOST_AUTO_TEST_SUITE_END()
