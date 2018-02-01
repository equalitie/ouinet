cmake_minimum_required (VERSION 3.5)
set(BOOST_VERSION 1.62)
include(ExternalProject)
################################################################################
# NOTE: https://stackoverflow.com/questions/37603238/fsanitize-not-using-gold-linker-in-gcc-6-1
set(SANITIZE "-fsanitize=address -fuse-ld=gold")
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

################################################################################
externalproject_add(beast
    GIT_REPOSITORY https://github.com/inetic/beast
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_COMMAND ""
    PREFIX "beast")

set(BEAST_DIR "${CMAKE_BINARY_DIR}/beast/src/beast")

################################################################################
option(IPFS_CACHE_WITH_EXAMPLE_BINARIES "" OFF)

add_subdirectory(./modules/i2pouiservice)
add_subdirectory(./modules/ipfs-cache)
add_subdirectory(./modules/gnunet-channels)

################################################################################
project(ouinet)

find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS filesystem
                                                        regex
                                                        unit_test_framework
                                                        program_options
                                                        coroutine
                                                        date_time
                                                        system)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

################################################################################
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${IPFS_CACHE_INCLUDE_DIR}"
    "${GNUNET_CHANNELS_INCLUDE_DIR}"
    "${I2POUI_INCLUDE_DIR}")

file(GLOB client_sources
    "./src/client.cpp"
    "./src/connect_to_host.cpp"
    "./src/client_front_end.cpp"
    "./src/endpoint.cpp"
    "./src/cache_control.cpp"
    "./src/request_routing.cpp")

add_executable(client ${client_sources})
add_dependencies(client beast ipfs-cache gnunet-channels i2poui)

target_link_libraries(client
    i2poui
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_LIBRARY}
    ${GNUNET_CHANNELS_LIBRARIES})

################################################################################
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${IPFS_CACHE_INCLUDE_DIR}"
    "${IPFS_CACHE_JSON_INCLUDE_DIR}"
    "${GNUNET_CHANNELS_INCLUDE_DIR}"
    "${I2POUI_INCLUDE_DIR}")

file(GLOB injector_sources
    "./src/injector.cpp"
    "./src/connect_to_host.cpp")

add_executable(injector ${injector_sources})
add_dependencies(injector beast ipfs-cache gnunet-channels i2poui)

target_link_libraries(injector
    i2poui
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_LIBRARY}
    ${GNUNET_CHANNELS_LIBRARIES})

################################################################################
# Tests
add_subdirectory(test)
################################################################################
