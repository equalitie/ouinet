cmake_minimum_required (VERSION 3.13)

link_libraries(ouinet_common)

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
add_executable(test-persistent-lru-cache "test_persistent_lru_cache.cpp")

################################################################################
add_executable(test-bittorrent "test_bittorrent.cpp")

################################################################################
add_executable(bt-bep44 "bt-bep44.cpp")

################################################################################
add_executable(test-routing-table "test-routing-table.cpp")

################################################################################
add_executable(bt-bep5 "bt-bep5.cpp")

######################################################################
add_executable(test-watch-dog "test_watch_dog.cpp")

######################################################################
add_executable(test-util "test-util.cpp")

set_target_properties(test-util PROPERTIES COMPILE_FLAGS "-fsanitize=address")
target_link_libraries(test-util asan)

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
add_executable(test-cache-announcer
    "performance_test/test_cache_announcer.cpp"
    "../test/util/bittorrent_utils.cpp")

add_executable(test-bencoding
    "test_bencoding.cpp"
    "../src/bittorrent/bencoding.cpp")

######################################################################
add_executable(test-file-io
    "test_file_io.cpp"
    "util/base_fixture.hpp"
)

######################################################################
add_executable(test-dht "test_dht.cpp")
