cmake_minimum_required (VERSION 2.8)
################################################################################
set(BOOST_VERSION 1.62)

################################################################################
option(WITH_BINARY "" OFF)
option(WITH_GUI "" OFF)
add_subdirectory(./i2pd/build)

################################################################################
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(IFADDRS_TARGET ifaddrs)
    add_library(${IFADDRS_TARGET} ${IFADDRS_SOURCES})
endif()

################################################################################
project(ouiservice-i2p)
find_package(OpenSSL REQUIRED)
find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -Wall -ggdb ${SANITIZE}")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "./i2pd/libi2pd"
    "./i2pd/libi2pd_client"
)

set(sources
    "./client.cpp"
    "./server.cpp"
    "./service.cpp"
)

add_library(ouiservice-i2p ${sources})
add_dependencies(ouiservice-i2p i2pd ${IFADDRS_TARGET})

target_link_libraries(ouiservice-i2p
    libi2pdclient
    libi2pd
    z
    ${OPENSSL_LIBRARIES}
    ${IFADDRS_TARGET}
    ${Boost_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

################################################################################
set(I2POUI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)
