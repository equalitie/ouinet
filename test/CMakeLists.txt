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
add_executable(test-parser "test_parser.cpp")

######################################################################
add_executable(test-cache
    "test_cache_control.cpp"
    "../src/cache_control.cpp"
)

######################################################################
add_executable(test-wait-condition "test_wait_condition.cpp")

######################################################################
add_executable(test-scheduler "test_scheduler.cpp")

######################################################################
add_executable(test-timeout-stream "test_timeout_stream.cpp")

######################################################################
add_executable(test-response-reader "test-response-reader.cpp")

######################################################################
add_executable(test-response-writer "test-response-writer.cpp")

################################################################################
if (NOT WIN32)
add_executable(test-persistent-lru-cache "test_persistent_lru_cache.cpp")
endif ()

################################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test-bittorrent "test_bittorrent.cpp")
endif()

################################################################################
if (NOT WIN32)
add_executable(bt-bep44 "bt-bep44.cpp")
endif ()

################################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test-routing-table "test-routing-table.cpp")
endif()

################################################################################
add_executable(bt-bep5 "bt-bep5.cpp")

######################################################################
add_executable(test-watch-dog "test_watch_dog.cpp")

######################################################################
add_executable(test-util "test-util.cpp")

if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set_target_properties(test-util PROPERTIES COMPILE_FLAGS "-fsanitize=address")
    if(${CMAKE_GENERATOR} STREQUAL "Xcode")
        set_target_properties(test-util PROPERTIES LINK_FLAGS "-fsanitize=address")
    else()
        target_link_libraries(test-util asan)
    endif()
endif ()

######################################################################
add_executable(test-logger "test_logger.cpp")

######################################################################
add_executable(test-connection-pool "test-connection-pool.cpp")

######################################################################
add_executable(oui-server "ouiservice-server.cpp")

######################################################################
add_executable(oui-client "ouiservice-client.cpp")

######################################################################
add_executable(test-http-util "test_http_util.cpp")

######################################################################
add_executable(test-http-sign
    "test_http_sign.cpp"
    "../src/cache/http_sign.cpp"
)

######################################################################
add_executable(test-http-store
    "test_http_store.cpp"
    "../src/cache/http_sign.cpp"
    "../src/cache/http_store.cpp"
    "../src/cache/hash_list.cpp"
    "../src/util/atomic_dir.cpp"
    "../src/util/temp_dir.cpp"
)

######################################################################
add_executable(test-atomic-temp
    "test_atomic_temp.cpp"
    "../src/util/atomic_dir.cpp"
    "../src/util/temp_dir.cpp"
)

######################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test-cache-announcer
        "performance_test/test_cache_announcer.cpp"
        "../test/util/bittorrent_utils.cpp")
endif()

add_executable(test-bencoding
    "test_bencoding.cpp"
    "../src/bittorrent/bencoding.cpp")

######################################################################
add_executable(test-file-io
    "test_file_io.cpp"
    "util/base_fixture.hpp"
)

######################################################################
if(NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
    add_executable(test-dht "test_dht.cpp")
endif()

######################################################################
add_executable(test-metrics "test-metrics.cpp")
