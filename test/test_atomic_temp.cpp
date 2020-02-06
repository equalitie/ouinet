#define BOOST_TEST_MODULE atomic_temp
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>

#include <util/temp_dir.h>


BOOST_AUTO_TEST_SUITE(ouinet_atomic_temp)

static const bool true_false[] = {true, false};

BOOST_DATA_TEST_CASE(test_temp_dir, boost::unit_test::data::make(true_false), keep) {
}

BOOST_AUTO_TEST_SUITE_END()
