cmake_minimum_required (VERSION 3.5)
set(GLOB BOOST_VERSION 1.55)
include(ExternalProject)
################################################################################
set(SANITIZE "-fsanitize=address")
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
set(IPFS_CACHE_SRC_DIR "${CMAKE_SOURCE_DIR}/modules/ipfs-cache")
set(IPFS_CACHE_BIN_DIR "${CMAKE_BINARY_DIR}/ipfs-cache-prefix/src/ipfs-cache-build")

externalproject_add(ipfs-cache
    SOURCE_DIR ${IPFS_CACHE_SRC_DIR}
    BUILD_ALWAYS 1
    DOWNLOAD_COMMAND ""
    INSTALL_COMMAND "")

################################################################################
project(client)

find_package(Boost ${BOOST_VERSION} COMPONENTS coroutine system REQUIRED)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${IPFS_CACHE_SRC_DIR}/include")

file(GLOB sources
    "${CMAKE_SOURCE_DIR}/src/client.cpp")

add_executable(client ${sources})
add_dependencies(client beast ipfs-cache)

target_link_libraries(client
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_BIN_DIR}/libipfs-cache.a)

################################################################################
project(injector)

find_package(Boost ${BOOST_VERSION} COMPONENTS coroutine system REQUIRED)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${IPFS_CACHE_SRC_DIR}/include")

file(GLOB sources
    "${CMAKE_SOURCE_DIR}/src/injector.cpp")

add_executable(injector ${sources})
add_dependencies(injector beast ipfs-cache)

target_link_libraries(injector
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_BIN_DIR}/libipfs-cache.a)

################################################################################
