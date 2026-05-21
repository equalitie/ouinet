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

function(simple_test test_name)
    add_executable(${test_name} ${test_name}.cpp ../src/util/log_path.cpp)
    target_link_libraries(${test_name} PUBLIC ouinet_asio Boost::unit_test_framework)
    target_include_directories(${test_name} PUBLIC ../src)
    target_compile_options(${test_name} PUBLIC -std=c++23)
endfunction()

function(common_test)
    set(options)
    set(oneValueArgs TARGET TARGET_SRC)
    set(multiValueArgs SRCS LIBS)

    cmake_parse_arguments(
        ARG
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    if (NOT ARG_TARGET_SRC)
        set(ARG_TARGET_SRC ${ARG_TARGET}.cpp)
    endif()

    add_executable(${ARG_TARGET} ${ARG_TARGET_SRC} ${ARG_SRCS})
    target_link_libraries(${ARG_TARGET} ${ARG_LIBS} ouinet_common Boost::unit_test_framework)
endfunction()

simple_test(test_cancel)

common_test(TARGET test_async)
common_test(TARGET test_parser)
common_test(TARGET test_cache_control LIBS client_lib injector_lib)
common_test(TARGET test_crypto_stream)
common_test(TARGET test_wait_condition)
common_test(TARGET test_scheduler)
common_test(TARGET test_timeout_stream)
common_test(TARGET test_response_reader)
common_test(TARGET test_response_writer)

if (NOT WIN32)
    common_test(TARGET test_persistent_lru_cache)
endif ()

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(TARGET test_bittorrent)
endif()

if (NOT WIN32)
    common_test(TARGET bt_bep44)
endif ()

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(TARGET test_routing_table)
endif()

common_test(TARGET bt_bep5)
common_test(TARGET test_watch_dog)
common_test(TARGET test_util)

if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set_target_properties(test_util PROPERTIES COMPILE_FLAGS "-fsanitize=address")
    if(${CMAKE_GENERATOR} STREQUAL "Xcode")
        set_target_properties(test_util PROPERTIES LINK_FLAGS "-fsanitize=address")
    else()
        target_link_libraries(test_util asan)
    endif()
endif ()

common_test(TARGET test_logger)
common_test(TARGET test_connection_pool)
common_test(TARGET test_http_util)
common_test(TARGET test_http_sign LIBS injector_lib)
common_test(TARGET test_http_store LIBS client_lib injector_lib)
common_test(TARGET test_atomic_temp LIBS client_lib)

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(TARGET test_cache_announcer
        TARGET_SRC "performance_test/test_cache_announcer.cpp"
        SRCS "../test/util/bittorrent_utils.cpp")
endif()

common_test(TARGET test_bencoding)

common_test(TARGET test_file_io)

if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(TARGET test_dht)
endif()

common_test(TARGET test_metrics)
common_test(TARGET test_injector_resolver LIBS injector_lib)
common_test(TARGET test_dns)
common_test(TARGET test_fetch LIBS injector_lib client_lib)

common_test(TARGET test_i2p)
common_test(TARGET test_i2p_direct)
common_test(TARGET test_i2p_endpoint)
common_test(TARGET test_i2p_fetch LIBS injector_lib client_lib)
common_test(TARGET test_i2p_tracker LIBS injector_lib client_lib)

if (WITH_OUISYNC)
    common_test(TARGET test_ouisync LIBS injector_lib client_lib)
endif()

if(UNIX AND NOT APPLE AND NOT CYGWIN)
    set(LINUX TRUE)
endif()

if(LINUX)
    common_test(TARGET manual_browse_scrape TARGET_SRC "manual/browse_scrape.cpp" LIBS client_lib)
endif()
