
include_directories(
    "${Boost_INCLUDE_DIR}"
    "../src")

add_executable(test-cache "test_cache_control.cpp"
                          "../src/cache_control.cpp"
                          "../src/asio.cpp"
                          "../src/logger.cpp")

target_link_libraries(test-cache ${Boost_LIBRARIES})

######################################################################
add_executable(test-wait-condition "test_wait_condition.cpp" "../src/asio.cpp")
target_link_libraries(test-wait-condition ${Boost_LIBRARIES})

######################################################################
add_executable(test-btree "test_btree.cpp"
                          "../src/cache/btree.cpp"
                          "../src/asio.cpp")
target_link_libraries(test-btree ${Boost_LIBRARIES})

######################################################################
add_executable(test-timeout-stream "test_timeout_stream.cpp"
                                   "../src/asio.cpp")

target_link_libraries(test-timeout-stream ${Boost_LIBRARIES})

################################################################################
find_path(GCRYPT_INCLUDE_DIR gcrypt.h)
find_library(GCRYPT_LIBRARIES NAMES gcrypt libgcrypt)

file(GLOB bt_cpp_files "../src/bittorrent/bencoding.cpp"
                       "../src/bittorrent/dht.cpp"
                       "../src/bittorrent/dht_storage.cpp"
                       "../src/bittorrent/mutable_data.cpp"
                       "../src/bittorrent/node_id.cpp"
                       "../src/bittorrent/routing_table.cpp"
                       "../src/util/crypto.cpp"
                       "../src/asio.cpp")

add_executable(test-bittorrent "test_bittorrent.cpp" ${bt_cpp_files})

target_include_directories(test-bittorrent PUBLIC
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-bittorrent ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES})

################################################################################
add_executable(test-dht "test-dht.cpp" ${bt_cpp_files})

target_include_directories(test-dht PUBLIC
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-dht ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES})

######################################################################
add_executable(test-logger "test_logger.cpp"
                           "../src/logger.cpp"
                           "../src/asio.cpp")
                           
target_link_libraries(test-logger ${Boost_LIBRARIES})
