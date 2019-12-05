#define BOOST_TEST_MODULE response_writer
#include <boost/test/included/unit_test.hpp>

#include "../src/response_writer.h"


using namespace std;
using namespace ouinet;

using RW = http_response::Writer;

namespace HR = http_response;


// Heads and trailers do not have default comparison operations,
// implement some dummy ones to be able to build.
namespace ouinet { namespace http_response {
    bool operator==(const HR::Head&, const HR::Head&) {
        return false;  // dummy
    }

    bool operator==(const HR::Trailer&, const HR::Trailer&) {
        return false;  // dummy
    }
}} // ouinet namespaces::http_response


BOOST_AUTO_TEST_SUITE(ouinet_response_writer)

BOOST_AUTO_TEST_CASE(test_dummy) {
    GenericStream out;
    RW rw(move(out));
}

BOOST_AUTO_TEST_SUITE_END()
