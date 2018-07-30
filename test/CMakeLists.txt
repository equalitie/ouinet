
include_directories(
    "${Boost_INCLUDE_DIR}"
    "../src")

################################################################################
add_executable(test-cache "test_cache_control.cpp" "../src/cache_control.cpp")
target_link_libraries(test-cache ${Boost_LIBRARIES})

################################################################################
add_executable(test-wait-condition "test_wait_condition.cpp")
target_link_libraries(test-wait-condition ${Boost_LIBRARIES})

################################################################################
add_executable(test-btree "test_btree.cpp" "../src/cache/btree.cpp")
target_link_libraries(test-btree ${Boost_LIBRARIES})

################################################################################
find_path(GCRYPT_INCLUDE_DIR gcrypt.h)
find_library(GCRYPT_LIBRARIES NAMES gcrypt libgcrypt)

file(GLOB bt_cpp_files "../src/bittorrent/bencoding.cpp"
                       "../src/bittorrent/dht.cpp"
                       "../src/bittorrent/dht_storage.cpp"
                       "../src/bittorrent/node_id.cpp"
                       "../src/bittorrent/routing_table.cpp"
                       "../src/util/crypto.cpp")

add_executable(test-bittorrent "test_bittorrent.cpp" ${bt_cpp_files})

target_include_directories(test-bittorrent PUBLIC
    "${Boost_INCLUDE_DIR}"
    "${GCRYPT_INCLUDE_DIR}")

target_link_libraries(test-bittorrent ${Boost_LIBRARIES} ${GCRYPT_LIBRARIES})
