cmake_minimum_required (VERSION 3.5)

include(ExternalProject)

################################################################################
option(WITH_BINARY "" OFF)
option(WITH_GUI "" OFF)
add_subdirectory(./i2pd/build)

# We link these libraries statically, which means their own dependencies must
# be listed explicitly on the libraries themselves rather than ouiservice-i2p.
set_target_properties(libi2pd PROPERTIES
    INTERFACE_LINK_LIBRARIES
        "Boost::date_time;Boost::filesystem;Boost::program_options;Boost::system;OpenSSL::Crypto;OpenSSL::SSL"
)
set_target_properties(libi2pdclient PROPERTIES
    INTERFACE_LINK_LIBRARIES
        "Boost::date_time;Boost::filesystem;Boost::program_options;Boost::system;OpenSSL::Crypto;OpenSSL::SSL"
)

# i2pd messes with CMAKE_MODULE_PATH, which breaks automatic dependency handling.
# Work around this issue by setting them by hand.
add_dependencies(libi2pd OpenSSL::Crypto OpenSSL::SSL Boost::boost)
add_dependencies(libi2pdclient OpenSSL::Crypto OpenSSL::SSL Boost::boost)

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
    externalproject_add(android-ifaddrs
        GIT_REPOSITORY https://github.com/PurpleI2P/android-ifaddrs
        GIT_TAG 8f9a87cd8d1a9c0cd1219f1ab9ef4ad1947cae30
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        PREFIX "android-ifaddrs"
        BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/android-ifaddrs/src/android-ifaddrs/ifaddrs.c
    )

    target_include_directories(libi2pd
        PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/android-ifaddrs/src/android-ifaddrs
    )
    add_dependencies(libi2pd android-ifaddrs)

    add_library(ifaddrs STATIC
        ${CMAKE_CURRENT_BINARY_DIR}/android-ifaddrs/src/android-ifaddrs/ifaddrs.c
    )
    add_dependencies(ifaddrs android-ifaddrs)
    set_target_properties(ifaddrs PROPERTIES LINKER_LANGUAGE C)
    target_link_libraries(ouiservice-i2p
        PRIVATE ifaddrs
    )
endif()
