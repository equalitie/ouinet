cmake_minimum_required (VERSION 3.13)

link_libraries(ouinet_common)

if(MINGW)
    # Add `#include <winsock2.h>` to every cpp file.
    # This gets rid of an error and a warning:
    #   error: #error WinSock.h has already been included
    #   warning: #warning Please include winsock2.h before windows.h
    # TODO: Only add to targets where needed or patch the file where it's
    # coming from (asio).
    add_definitions(-include winsock2.h)
endif()


######################################################################
add_executable(test_parser "test_parser.cpp")

######################################################################
add_executable(test_cache "test_cache_control.cpp")
target_link_libraries(test_cache client_lib injector_lib)

######################################################################
add_executable(test_wait_condition "test_wait_condition.cpp")

######################################################################
add_executable(test_scheduler "test_scheduler.cpp")

######################################################################
add_executable(test_timeout_stream "test_timeout_stream.cpp")

######################################################################
add_executable(test_response_reader "test_response_reader.cpp")

######################################################################
add_executable(test_response_writer "test_response_writer.cpp")

################################################################################
if (NOT WIN32)
add_executable(test_persistent_lru_cache "test_persistent_lru_cache.cpp")
endif ()

################################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test_bittorrent "test_bittorrent.cpp")
endif()

################################################################################
if (NOT WIN32)
add_executable(bt_bep44 "bt_bep44.cpp")
endif ()

################################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test_routing_table "test_routing_table.cpp")
endif()

################################################################################
add_executable(bt_bep5 "bt_bep5.cpp")

######################################################################
add_executable(test_watch_dog "test_watch_dog.cpp")

######################################################################
add_executable(test_util "test_util.cpp")

if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set_target_properties(test_util PROPERTIES COMPILE_FLAGS "-fsanitize=address")
    if(${CMAKE_GENERATOR} STREQUAL "Xcode")
        set_target_properties(test_util PROPERTIES LINK_FLAGS "-fsanitize=address")
    else()
        target_link_libraries(test_util asan)
    endif()
endif ()

######################################################################
add_executable(test_logger "test_logger.cpp")

######################################################################
add_executable(test_connection_pool "test_connection_pool.cpp")

######################################################################
add_executable(oui_server "ouiservice_server.cpp")

######################################################################
add_executable(oui_client "ouiservice_client.cpp")

######################################################################
add_executable(test_http_util "test_http_util.cpp")

######################################################################
add_executable(test_http_sign
    "test_http_sign.cpp"
    "../src/cache/http_sign.cpp"
)

######################################################################
add_executable(test_http_store "test_http_store.cpp")
target_link_libraries(test_http_store client_lib)

######################################################################
add_executable(test_atomic_temp
    "test_atomic_temp.cpp"
    "../src/util/atomic_dir.cpp"
    "../src/util/temp_dir.cpp"
)

######################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test_cache_announcer
        "performance_test/test_cache_announcer.cpp"
        "../test/util/bittorrent_utils.cpp")
endif()

add_executable(test_bencoding
    "test_bencoding.cpp"
    "../src/bittorrent/bencoding.cpp")

######################################################################
add_executable(test_file_io
    "test_file_io.cpp"
    "util/base_fixture.hpp"
)

######################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test_dht "test_dht.cpp")
endif()

######################################################################
add_executable(test_metrics "test_metrics.cpp")

######################################################################
add_executable(test_injector_resolver "test_injector_resolver.cpp")
target_link_libraries(test_injector_resolver injector_lib)

######################################################################
add_executable(test_dns "test_dns.cpp")

######################################################################
add_executable(test_fetch "test_fetch.cpp")
target_link_libraries(test_fetch injector_lib client_lib)

######################################################################
if (WITH_OUISYNC)
    add_executable(test_ouisync "test_ouisync.cpp")
    target_link_libraries(test_ouisync injector_lib client_lib)
endif()
