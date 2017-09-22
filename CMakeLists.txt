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
project(ouinet)

find_package(Boost ${BOOST_VERSION} COMPONENTS coroutine system REQUIRED)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include")

file(GLOB sources
    "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(ouinet ${sources})

target_link_libraries(ouinet ${Boost_LIBRARIES})

################################################################################
