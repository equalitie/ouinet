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
    add_executable(${test_name} ${ARGN})
    target_link_libraries(${test_name} PUBLIC ouinet_asio)
    target_include_directories(${test_name} PUBLIC ../src)
    target_compile_options(${test_name} PUBLIC -std=c++23)
endfunction()

function(common_test test_name)
    add_executable(${test_name} ${ARGN})
    target_link_libraries(${test_name} ouinet_common)
endfunction()

######################################################################
simple_test(test_cancel "test_cancel.cpp")

######################################################################
common_test(test_parser "test_parser.cpp")

######################################################################
common_test(test_cache "test_cache_control.cpp")
target_link_libraries(test_cache client_lib injector_lib)

######################################################################
common_test(test_crypto_stream "test_crypto_stream.cpp")

######################################################################
common_test(test_wait_condition "test_wait_condition.cpp")

######################################################################
common_test(test_scheduler "test_scheduler.cpp")

######################################################################
common_test(test_timeout_stream "test_timeout_stream.cpp")

######################################################################
common_test(test_response_reader "test_response_reader.cpp")
target_link_libraries(test_response_reader ouinet_common)

######################################################################
common_test(test_response_writer "test_response_writer.cpp")

######################################################################
if (NOT WIN32)
    common_test(test_persistent_lru_cache "test_persistent_lru_cache.cpp")
endif ()

################################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(test_bittorrent "test_bittorrent.cpp")
    target_link_libraries(test_bittorrent ouinet_common)
endif()

################################################################################
if (NOT WIN32)
    common_test(bt_bep44 "bt_bep44.cpp")
endif ()

################################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(test_routing_table "test_routing_table.cpp")
endif()

################################################################################
common_test(bt_bep5 "bt_bep5.cpp")

######################################################################
common_test(test_watch_dog "test_watch_dog.cpp")

######################################################################
common_test(test_util "test_util.cpp")

if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set_target_properties(test_util PROPERTIES COMPILE_FLAGS "-fsanitize=address")
    if(${CMAKE_GENERATOR} STREQUAL "Xcode")
        set_target_properties(test_util PROPERTIES LINK_FLAGS "-fsanitize=address")
    else()
        target_link_libraries(test_util asan)
    endif()
endif ()

######################################################################
common_test(test_logger "test_logger.cpp")

######################################################################
common_test(test_connection_pool "test_connection_pool.cpp")

######################################################################
common_test(oui_server "ouiservice_server.cpp")

######################################################################
common_test(oui_client "ouiservice_client.cpp")

######################################################################
common_test(test_http_util "test_http_util.cpp")

######################################################################
common_test(test_http_sign "test_http_sign.cpp")
target_link_libraries(test_http_sign injector_lib)

######################################################################
common_test(test_http_store "test_http_store.cpp")
target_link_libraries(test_http_store client_lib injector_lib)

######################################################################
common_test(test_atomic_temp "test_atomic_temp.cpp")
target_link_libraries(test_atomic_temp client_lib)

######################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(test_cache_announcer
        "performance_test/test_cache_announcer.cpp"
        "../test/util/bittorrent_utils.cpp")
endif()

######################################################################
common_test(test_bencoding "test_bencoding.cpp")
target_link_libraries(test_bencoding ouinet_common)

######################################################################
common_test(test_file_io "test_file_io.cpp" "util/base_fixture.hpp")

######################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    common_test(test_dht "test_dht.cpp")
endif()

######################################################################
common_test(test_metrics "test_metrics.cpp")

######################################################################
common_test(test_injector_resolver "test_injector_resolver.cpp")
target_link_libraries(test_injector_resolver injector_lib)

######################################################################
common_test(test_dns "test_dns.cpp")

######################################################################
common_test(test_fetch "test_fetch.cpp")
target_link_libraries(test_fetch injector_lib client_lib)

######################################################################
if(WITH_EXPERIMENTAL)
    common_test(test_i2p test_i2p.cpp)
    target_link_libraries(test_i2p client_lib)

    common_test(test_i2p_endpoint "test_i2p_endpoint.cpp")
    target_link_libraries(test_i2p_endpoint injector_lib)

    common_test(test_i2p_fetch "./test_i2p_fetch.cpp")
    target_link_libraries(test_i2p_fetch injector_lib client_lib)

    common_test(test_i2p_tracker ./test_i2p_tracker.cpp)
    target_link_libraries(test_i2p_tracker injector_lib client_lib)
endif()

if (WITH_OUISYNC)
    common_test(test_ouisync "test_ouisync.cpp")
    target_link_libraries(test_ouisync injector_lib client_lib)
endif()

if(UNIX AND NOT APPLE AND NOT CYGWIN)
    set(LINUX TRUE)
endif()

if(LINUX)
    common_test(manual_browse_scrape "manual/browse_scrape.cpp")
    target_link_libraries(manual_browse_scrape client_lib)
endif()
