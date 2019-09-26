cmake_minimum_required (VERSION 3.13)

include_directories(
    "../src"
)
link_libraries(
    Boost::iostreams
    Boost::system
    ouinet::base
)

######################################################################
add_executable(test-parser "test_parser.cpp")

######################################################################
add_executable(test-cache
    "test_cache_control.cpp"
    "../src/cache_control.cpp"
    "../src/http_util.cpp"
    "../src/logger.cpp"
)
target_link_libraries(test-cache lib::uri)

######################################################################
add_executable(test-wait-condition "test_wait_condition.cpp")

######################################################################
add_executable(test-scheduler "test_scheduler.cpp")

######################################################################
add_executable(test-timeout-stream "test_timeout_stream.cpp")

######################################################################
add_executable(test-stream-fork "test-stream-fork.cpp")

################################################################################
add_executable(test-persistent-lru-cache
    "test_persistent_lru_cache.cpp"
    "../src/util/persistent_lru_cache.cpp"
    "../src/util/hash.cpp"
    "../src/util/atomic_file.cpp"
    "../src/util/crypto.cpp"
    "../src/util/file_io.cpp"
    "../src/util/temp_file.cpp"
)
target_link_libraries(test-persistent-lru-cache lib::gcrypt)

################################################################################
file(GLOB bt_cpp_files
    "../src/bittorrent/*.cpp"
    "../src/util/crypto.cpp"
    "../src/util/hash.cpp"
    "../src/logger.cpp"
    "../src/util.cpp"
    "../src/util/file_io.cpp"
)

add_executable(test-bittorrent "test_bittorrent.cpp" ${bt_cpp_files})
target_link_libraries(test-bittorrent lib::asio_utp lib::gcrypt)

################################################################################
add_executable(bt-bep44 "bt-bep44.cpp" ${bt_cpp_files})
target_link_libraries(bt-bep44 lib::asio_utp lib::gcrypt z)

################################################################################
add_executable(test-routing-table "test-routing-table.cpp" ${bt_cpp_files})
target_link_libraries(test-routing-table lib::asio_utp lib::gcrypt z)

################################################################################
add_executable(bt-bep5 "bt-bep5.cpp" ${bt_cpp_files})
target_link_libraries(bt-bep5 lib::asio_utp lib::gcrypt)

######################################################################
add_executable(test-watch-dog "test_watch_dog.cpp")

######################################################################
add_executable(test-util "test-util.cpp")
set_target_properties(test-util PROPERTIES COMPILE_FLAGS "-fsanitize=address")
# CircleCI still includes an ancient version of GCC 6,
# so we need to use the gold linker to enable ASan,
# see <https://stackoverflow.com/q/37603238>.
target_link_options(test-util PUBLIC "-fsanitize=address" "-fuse-ld=gold")
target_link_libraries(test-util asan)

######################################################################
add_executable(test-logger "test_logger.cpp" "../src/logger.cpp")

######################################################################
add_executable(test-connection-pool "test-connection-pool.cpp")

######################################################################
add_executable(oui-server
    "ouiservice-server.cpp"
    "../src/logger.cpp"
    "../src/endpoint.cpp"
    "../src/ouiservice.cpp"
    "../src/ouiservice/tcp.cpp"
)

######################################################################
add_executable(oui-client
    "ouiservice-client.cpp"
    "../src/logger.cpp"
    "../src/endpoint.cpp"
    "../src/ouiservice.cpp"
    "../src/ouiservice/tcp.cpp"
)

######################################################################
add_executable(test-http-util "test_http_util.cpp")

######################################################################
add_executable(test-http-sign
    "test_http_sign.cpp"
    "../src/cache/http_sign.cpp"
    "../src/logger.cpp"
    "../src/util.cpp"
    "../src/util/crypto.cpp"
    "../src/util/hash.cpp"
)
target_link_libraries(test-http-sign lib::gcrypt z)
