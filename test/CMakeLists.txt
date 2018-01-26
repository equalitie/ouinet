
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "../src")

file(GLOB test_sources "*.cpp" "../src/cache_control.cpp")

add_executable(test-ouinet ${test_sources})
add_dependencies(test-ouinet beast)

target_link_libraries(test-ouinet
    ${Boost_LIBRARIES})
