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
set(GNUNET_CHANNELS_SRC_DIR "${CMAKE_SOURCE_DIR}/modules/gnunet-channels")
set(GNUNET_CHANNELS_ROOT    "${CMAKE_BINARY_DIR}/gnunet-channels/src/gnunet-channels-build")
set(GNUNET_LIB_DIR          "${GNUNET_CHANNELS_ROOT}/gnunet/lib")

externalproject_add(gnunet-channels
    SOURCE_DIR ${GNUNET_CHANNELS_SRC_DIR}
    BUILD_ALWAYS 1
    DOWNLOAD_COMMAND ""
    INSTALL_COMMAND ""
    PREFIX "gnunet-channels")

################################################################################
project(client)

find_package(Boost ${BOOST_VERSION} COMPONENTS program_options coroutine system REQUIRED)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${IPFS_CACHE_SRC_DIR}/include"
    "${GNUNET_CHANNELS_SRC_DIR}/include")

file(GLOB sources
    "${CMAKE_SOURCE_DIR}/src/client.cpp"
    "${CMAKE_SOURCE_DIR}/src/connect_to_host.cpp"
    "${CMAKE_SOURCE_DIR}/src/client_front_end.cpp"
    "${CMAKE_SOURCE_DIR}/src/request_routing.cpp")

add_executable(client ${sources})
add_dependencies(client beast ipfs-cache gnunet-channels)

target_link_libraries(client
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_BIN_DIR}/libipfs-cache.a
    ${GNUNET_CHANNELS_ROOT}/libgnunet-channels.a
    ${GNUNET_LIB_DIR}/libgnunethello.so
    ${GNUNET_LIB_DIR}/libgnunettransport.so
    ${GNUNET_LIB_DIR}/libgnunetutil.so
    ${GNUNET_LIB_DIR}/libgnunetcadet.so)

################################################################################
project(injector)

find_package(Boost ${BOOST_VERSION} COMPONENTS program_options coroutine system REQUIRED)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${IPFS_CACHE_BIN_DIR}/json/src/json/src"
    "${IPFS_CACHE_SRC_DIR}/include")

file(GLOB sources
    "${CMAKE_SOURCE_DIR}/src/injector.cpp"
    "${CMAKE_SOURCE_DIR}/src/connect_to_host.cpp")

add_executable(injector ${sources})
add_dependencies(injector beast ipfs-cache)

target_link_libraries(injector
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_BIN_DIR}/libipfs-cache.a
    ${GNUNET_CHANNELS_ROOT}/libgnunet-channels.a
    ${GNUNET_LIB_DIR}/libgnunethello.so
    ${GNUNET_LIB_DIR}/libgnunettransport.so
    ${GNUNET_LIB_DIR}/libgnunetutil.so
    ${GNUNET_LIB_DIR}/libgnunetcadet.so)

################################################################################
