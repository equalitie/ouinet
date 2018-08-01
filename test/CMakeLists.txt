
include_directories(
    "${Boost_INCLUDE_DIR}"
    "../src")

######################################################################
add_executable(test-cache "test_cache_control.cpp"
                          "../src/cache_control.cpp"
                          "../src/asio.cpp")
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
add_executable(test-bittorrent "test_bittorrent.cpp"
                               "../src/bittorrent/node_id.cpp"
                               "../src/asio.cpp")
target_link_libraries(test-bittorrent ${Boost_LIBRARIES})
