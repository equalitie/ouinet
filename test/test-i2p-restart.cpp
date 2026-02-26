#include "../test/util/i2p_utils.hpp"

BOOST_AUTO_TEST_SUITE(ouinet_i2p)

BOOST_AUTO_TEST_CASE(test_connect_and_exchange) {
    Setup setup;
    asio::io_context ctx;

    // shared_ptr<Service> service(make_shared<Service>(setup.tempdir.string(), ctx.get_executor()));
    {
        Service service(setup.tempdir.string(), ctx.get_executor());
    }
    {
        Service service(setup.tempdir.string(), ctx.get_executor());
    }

}

BOOST_AUTO_TEST_SUITE_END()
