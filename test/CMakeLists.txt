cmake_minimum_required (VERSION 3.13)

if(MINGW)
    # Add `#include <winsock2.h>` to every cpp file.
    # This gets rid of an error and a warning:
    #   error: #error WinSock.h has already been included
    #   warning: #warning Please include winsock2.h before windows.h
    # TODO: Only add to targets where needed or patch the file where it's
    # coming from (asio).
    add_definitions(-include winsock2.h)
endif()

set(OUINET_TEST_TARGETS)

add_library(ouinet_test_utils OBJECT ./util/http_server.cpp)
ouinet_setup(TARGET ouinet_test_utils LINK ouinet_asio)

function(add_test)
    set(options)
    set(oneValueArgs TARGET TYPE TARGET_SRC)
    set(multiValueArgs SRCS)

    cmake_parse_arguments(
        ARG
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    set(tmp ${OUINET_TEST_TARGETS})
    list(APPEND tmp ${ARG_TARGET})
    set(OUINET_TEST_TARGETS ${tmp} PARENT_SCOPE)

    if (NOT ARG_TARGET_SRC)
        set(ARG_TARGET_SRC ${ARG_TARGET}.cpp)
    endif()

    add_executable(${ARG_TARGET} ${ARG_TARGET_SRC} ${ARG_SRCS})

    if (ARG_TYPE STREQUAL "HEADER")
        target_link_libraries(${ARG_TARGET} PUBLIC ouinet_asio Boost::unit_test_framework)
        target_include_directories(${ARG_TARGET} PUBLIC ../src)
        target_compile_options(${ARG_TARGET} PUBLIC -std=c++23)
    else ()
        ouinet_setup(TARGET ${ARG_TARGET}
            LINK
                ouinet_client
                ouinet_injector
                ouinet_rs
                ouinet_test_utils
                Boost::unit_test_framework
        )
    endif()

    if (WITH_MOCK_DHT)
        target_compile_definitions(${ARG_TARGET} PRIVATE WITH_MOCK_DHT)
    endif()
endfunction()

add_test(TARGET test_cancel TYPE HEADER)
add_test(TARGET test_promise TYPE HEADER)

add_test(TARGET test_async)
add_test(TARGET test_parser)
add_test(TARGET test_cache_control)
add_test(TARGET test_crypto_stream)
add_test(TARGET test_wait_condition)
add_test(TARGET test_select)
add_test(TARGET test_scheduler)
add_test(TARGET test_timeout_stream)
add_test(TARGET test_response_reader)
add_test(TARGET test_response_writer)

if (NOT WIN32)
    add_test(TARGET test_persistent_lru_cache)
endif ()

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_test(TARGET test_bittorrent)
endif()

if (NOT WIN32)
    add_test(TARGET bt_bep44)
endif ()

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_test(TARGET test_routing_table)
endif()

add_test(TARGET bt_bep5)
add_test(TARGET test_watch_dog)
add_test(TARGET test_util)
add_test(TARGET test_logger)
add_test(TARGET test_connection_pool)
add_test(TARGET test_http_util)
add_test(TARGET test_http_sign)
add_test(TARGET test_http_store)
add_test(TARGET test_atomic_temp)

# TODO: This one uses dirty tricks and needs to be refactored:
#   * It `#include`s a cpp file
#   * It redefines `private` to `public` to access some data
#if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
#    add_test(TARGET test_cache_announcer
#        TARGET_SRC "performance_test/test_cache_announcer.cpp"
#        SRCS "../test/util/bittorrent_utils.cpp"
#       )
#endif()

add_test(TARGET test_bencoding)

add_test(TARGET test_file_io)

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_test(TARGET test_dht)
endif()

add_test(TARGET test_metrics)
add_test(TARGET test_injector_resolver)
add_test(TARGET test_dns)
add_test(TARGET test_fetch)

add_test(TARGET test_i2p)
add_test(TARGET test_i2p_endpoint)
add_test(TARGET test_i2p_fetch)
add_test(TARGET test_i2p_tracker)

if (WITH_OUISYNC)
    add_test(TARGET test_ouisync)
endif()

if(UNIX AND NOT APPLE AND NOT CYGWIN)
    set(LINUX TRUE)
endif()

if(LINUX)
    add_test(TARGET manual_browse_scrape TARGET_SRC "manual/browse_scrape.cpp")
endif()

######################################################################

if (LINUX)
    include(rust/cmake/rust_test.cmake)
    add_rust_test(test_connectivity)
endif()

######################################################################
string(REPLACE ";" " " OUINET_TEST_TARGETS_STR "tests: ${OUINET_TEST_TARGETS}")
add_custom_target(list_test_targets
    #COMMAND ${CMAKE_COMMAND} -E echo "${OUINET_TEST_TARGETS_STR}"
    COMMAND echo "${OUINET_TEST_TARGETS_STR}"
    VERBATIM
    USES_TERMINAL)
