cmake_minimum_required (VERSION 3.5)
set(BOOST_VERSION 1.62)
include(ExternalProject)
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
add_dependencies(client beast ${IPFS_CACHE_TARGET} gnunet-channels i2poui)

target_link_libraries(client
    i2poui i2pdclient libi2pd
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_LIBRARIES}
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
    "./src/connect_to_host.cpp"
    "./src/cache_control.cpp")

add_executable(injector ${injector_sources})
add_dependencies(injector beast ${IPFS_CACHE_TARGET} gnunet-channels i2poui)

target_link_libraries(injector
    i2poui i2pdclient libi2pd
    ${Boost_LIBRARIES}
    ${IPFS_CACHE_LIBRARIES}
    ${GNUNET_CHANNELS_LIBRARIES})

################################################################################
# Config file directory

set(config_files
    "repos/client/ouinet-client.conf"
    "repos/client/gnunet/peer.conf"
    "repos/injector/ouinet-injector.conf"
    "repos/injector/gnunet/peer.conf"
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
