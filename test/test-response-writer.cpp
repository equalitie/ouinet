#define BOOST_TEST_MODULE response_writer
#include <boost/test/included/unit_test.hpp>

#include "../src/response_writer.h"


using namespace std;
using namespace ouinet;

using RW = http_response::Writer;


BOOST_AUTO_TEST_SUITE(ouinet_response_writer)

BOOST_AUTO_TEST_CASE(test_dummy) {
    GenericStream out;
    RW rw(move(out));
}

BOOST_AUTO_TEST_SUITE_END()
