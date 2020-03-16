#define BOOST_TEST_MODULE logger_tester
#include <boost/test/included/unit_test.hpp>

#include <iostream>
#include <string>

#include "namespaces.h"
#include "logger.h"

BOOST_AUTO_TEST_SUITE(logger_tester)

using namespace std;
using namespace ouinet;


BOOST_AUTO_TEST_CASE(test_generate_node_id)
{    
    Logger log(SILLY);                // All logs with level >= SILLY will display

    // Do some logging
    // You can call log with a level manually
    log.log(INFO, "This is the first info log");
    log.log(SILLY, "This is the first silly log");

    // Or you can use a convenience method, named after the log level
    log.warn("This is the first warning");
    log.error("This is the first error");

    // Or you can use macros for efficiency
    LOG_DEBUG("This is a macro DEBUG LOG");
    LOG_ERROR("This is a macro ERROR LOG");

    // Set the log threshold to something higher.
    // Now we will only see logs with a level >= VERBOSE
    log.set_threshold(VERBOSE);
    log.verbose("This should make it out");
    log.warn("This should make it out");
    log.debug("This should not make it out");
    log.silly("This should not make it out");

    logger.set_threshold(VERBOSE);
    LOG_VERBOSE("This should make it out from the default logger with the macro");
    LOG_WARN("This should make it out with from the default logger the macro");
    LOG_DEBUG("This should not make it out from the default logger with the macro");
}

BOOST_AUTO_TEST_SUITE_END()
