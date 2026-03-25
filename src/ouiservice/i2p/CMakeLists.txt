cmake_minimum_required (VERSION 3.5)

include(ExternalProject)

################################################################################
block()
    set(WITH_BINARY OFF)
    set(WITH_GUI OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    add_definitions(-DBOOST_ASIO_DYN_LINK)
    add_subdirectory(./i2pd/build)
    if(POLICY CMP0079)
        # Allows adding the libraries below
        # https://stackoverflow.com/a/75246115/273348
        cmake_policy(SET CMP0079 NEW)
    endif()
    target_link_libraries(libi2pd       PRIVATE Boost::program_options ouinet_asio ouinet_asio_ssl)
    target_link_libraries(libi2pdclient PRIVATE Boost::program_options ouinet_asio ouinet_asio_ssl)
    target_link_libraries(libi2pdlang   PRIVATE                        ouinet_asio ouinet_asio_ssl)
endblock()

# i2pd messes with CMAKE_MODULE_PATH, which breaks automatic dependency handling.
# Work around this issue by setting them by hand.
add_dependencies(libi2pd       OpenSSL::Crypto OpenSSL::SSL OpenSSL::headers Boost::boost)
add_dependencies(libi2pdclient OpenSSL::Crypto OpenSSL::SSL OpenSSL::headers Boost::boost)
add_dependencies(libi2pdlang                                OpenSSL::headers Boost::boost)

################################################################################
project(ouiservice-i2p)

add_library(ouiservice-i2p SHARED
    "./tunnel.cpp"
    "./handshake.cpp"
    "./client.cpp"
    "./server.cpp"
    "./service.cpp"
    "../../logger.cpp"
)
add_library(ouinet::ouiservice-i2p ALIAS ouiservice-i2p)
target_include_directories(ouiservice-i2p
    PUBLIC
        "./i2pd/libi2pd"
        "./i2pd/libi2pd_client"
        "./i2pd/i18n"
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ouiservice-i2p
    PRIVATE
        libi2pdclient
        libi2pd
        z
        libi2pdlang
        OpenSSL::SSL
    PUBLIC
        ouinet_common
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
