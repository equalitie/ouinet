cmake_minimum_required (VERSION 3.5)

################################################################################
option(WITH_BINARY "" OFF)
option(WITH_GUI "" OFF)
add_subdirectory(./i2pd/build)

# We link these libraries statically, which means their own dependencies must
# be listed explicitly on the libraries themselves rather than ouiservice-i2p.
set_target_properties(libi2pd PROPERTIES
    INTERFACE_LINK_LIBRARIES
        "Boost::date_time;Boost::filesystem;Boost::program_options;Boost::system"
)
set_target_properties(libi2pdclient PROPERTIES
    INTERFACE_LINK_LIBRARIES
        "Boost::date_time;Boost::filesystem;Boost::program_options;Boost::system"
)

################################################################################
project(ouiservice-i2p)

add_library(ouiservice-i2p
    "./tunnel.cpp"
    "./client.cpp"
    "./server.cpp"
    "./service.cpp"
    "../../logger.cpp"
)
add_library(ouinet::ouiservice-i2p ALIAS ouiservice-i2p)
target_include_directories(ouiservice-i2p
    PRIVATE
        "./i2pd/libi2pd"
        "./i2pd/libi2pd_client"
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ouiservice-i2p
    PRIVATE
        libi2pdclient
        libi2pd
        z
        OpenSSL::SSL
    PUBLIC
        ouinet::base
)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_library(ifaddrs ${IFADDRS_SOURCES})
    target_link_libraries(ouiservice-i2p
        PRIVATE ifaddrs
    )
endif()
