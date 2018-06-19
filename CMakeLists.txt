if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # CMAKE_ANDROID_STANDALONE_TOOLCHAIN (and likely other cross compilation
    # android variables) was only introduced in v3.7
    cmake_minimum_required (VERSION 3.7)
else()
    cmake_minimum_required (VERSION 3.5)
endif()

set(BOOST_VERSION 1.62)
include(ExternalProject)
################################################################################
# Configurable options
option(WITH_INJECTOR "Compile injector" ON)

################################################################################
# NOTE: https://stackoverflow.com/questions/37603238/fsanitize-not-using-gold-linker-in-gcc-6-1
# NOTE2: Need to disable ASan until Boost.Asio switches to Boost.Coroutine2
#        because trowing (and catching) exceptions inside a coroutine confuses
#        ASan. More info:
#        https://github.com/boostorg/coroutine/issues/30#issuecomment-325574878
#set(SANITIZE "-fsanitize=address -fuse-ld=gold")

# For some reason we need to define both of these
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)
add_definitions(-DBOOST_COROUTINE_NO_DEPRECATION_WARNING)

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
option(ASIO_IPFS_WITH_EXAMPLE_BINARIES "" OFF)

add_subdirectory(./modules/asio-ipfs)
add_subdirectory(./src/ouiservice/i2p)

################################################################################
project(ouinet)

find_package(OpenSSL REQUIRED)
find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS filesystem
                                                        regex
                                                        unit_test_framework
                                                        program_options
                                                        coroutine
                                                        context
                                                        thread
                                                        date_time
                                                        system)
find_package(Threads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread -Wall -ggdb ${SANITIZE}")

################################################################################
externalproject_add(json
    # TODO: We only really need to download one header file.
    URL https://github.com/nlohmann/json/archive/v2.1.1.tar.gz
    BUILD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    PREFIX json)

set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/json/src/json/src")

################################################################################
include_directories(
    "${Boost_INCLUDE_DIR}"
    "${BEAST_DIR}/include"
    "${ASIO_IPFS_INCLUDE_DIR}"
    "${JSON_INCLUDE_DIR}"
)

file(GLOB client_sources
    "./src/client.cpp"
    "./src/connect_to_host.cpp"
    "./src/client_front_end.cpp"
    "./src/endpoint.cpp"
    "./src/cache_control.cpp"
    "./src/request_routing.cpp"
    "./src/ouiservice.cpp"
    "./src/ouiservice/tcp.cpp"
    "./src/logger.cpp"
    "./src/cache/*.cpp"
)

if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_executable(client ${client_sources})
else()
    find_library( # Sets the name of the path variable.
                  log-lib
                  # Specifies the name of the NDK library that
                  # you want CMake to locate.
                  log )
    add_library(client SHARED ${client_sources})
endif()

target_link_libraries(client
    ${log-lib}
    ouiservice-i2p
    ${Boost_LIBRARIES}
    ${ASIO_IPFS_LIBRARIES}
)

add_dependencies(client json beast ${ASIO_IPFS_TARGET})

################################################################################
if (WITH_INJECTOR)
    include_directories(
        "${Boost_INCLUDE_DIR}"
        "${BEAST_DIR}/include"
        "${ASIO_IPFS_INCLUDE_DIR}"
    )

    file(GLOB injector_sources
        "./src/injector.cpp"
        "./src/connect_to_host.cpp"
        "./src/cache_control.cpp"
        "./src/ouiservice.cpp"
        "./src/ouiservice/tcp.cpp"
        "./src/logger.cpp"
        "./src/cache/*.cpp"
    )

    add_executable(injector ${injector_sources})
    add_dependencies(injector json beast ${ASIO_IPFS_TARGET})

    target_link_libraries(injector
        ouiservice-i2p
        ${Boost_LIBRARIES}
        ${ASIO_IPFS_LIBRARIES}
    )
endif() # if WITH_INJECTOR

################################################################################
# Temporary, until we have proper dht tests. At which point it should move
# to the ./test/ directory.

file(GLOB test_dht_sources
    "./src/bittorrent/*.cpp"
)

add_executable(test-dht ${test_dht_sources})

target_include_directories(test-dht PUBLIC
    "${Boost_INCLUDE_DIR}"
)

target_link_libraries(test-dht ${Boost_LIBRARIES})

################################################################################
# Config file directory

set(config_files
    "repos/client/ouinet-client.conf"
    "repos/injector/ouinet-injector.conf"
)
set(config_targets )

if(NOT ${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    add_custom_target(config ALL DEPENDS test)
    foreach(config_file ${config_files})
        get_filename_component(config_dir ${config_file} DIRECTORY)
        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/${config_file}
            MAIN_DEPENDENCY ${CMAKE_SOURCE_DIR}/${config_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/${config_dir}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/${config_file} ${CMAKE_BINARY_DIR}/${config_file}
        )
        set(config_targets ${config_targets} ${CMAKE_BINARY_DIR}/${config_file})
    endforeach()
    add_custom_target(appel ALL DEPENDS ${config_targets})
endif()

################################################################################
# Tests
add_subdirectory(test)
################################################################################
