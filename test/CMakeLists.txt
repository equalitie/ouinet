
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "../src")

file(GLOB test_cache_src "test_cache_control.cpp" "../src/cache_control.cpp")
add_executable(test-cache ${test_cache_src})
add_dependencies(test-cache beast)
target_link_libraries(test-cache ${Boost_LIBRARIES})

file(GLOB test_blocker_src "test_blocker.cpp")
add_executable(test-blocker ${test_blocker_src})
target_link_libraries(test-blocker ${Boost_LIBRARIES})
