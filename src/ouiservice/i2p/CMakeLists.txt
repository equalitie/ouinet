cmake_minimum_required (VERSION 3.13)

include(ExternalProject)

################################################################################
if (WITH_I2PD)
    block()
        set(WITH_BINARY ON)
        set(WITH_LIBRARY OFF)
        set(WITH_GUI OFF)
        set(BUILD_TESTING OFF)
    
        if (MINGW)
            set(WITH_STATIC ON)
    
            # HACK: `find_package(ZLIB)` used by i2pd can't find pre-installed
            # zlib on msys64 for some reason.
            find_package(ZLIB QUIET)
            if(NOT ZLIB_FOUND)
                set(ZLIB_LIBRARY c:/msys64/usr/lib/libz.a)
                set(ZLIB_INCLUDE_DIR c:/msys64/usr/include)
            endif()
        endif()
    
        add_subdirectory(./i2pd/build)
    endblock()
    
    # We build our own boost and openssl, so make sure those are built before i2pd
    # starts building so it can use those. Normally this should be automatic, but i2pd
    # does not specify that the later two libraries depend on Boost.
    if (TARGET built_boost)
        add_dependencies(libi2pd built_boost)
        add_dependencies(libi2pdclient built_boost)
        add_dependencies(libi2pdlang built_boost)
    endif()
    if (TARGET built_openssl)
        add_dependencies(libi2pd built_openssl)
        add_dependencies(libi2pdclient built_openssl)
        add_dependencies(libi2pdlang built_openssl)
    endif()
    
    if(POLICY CMP0079)
        # Allows adding the libraries below
        # https://stackoverflow.com/a/75246115/273348
        cmake_policy(SET CMP0079 NEW)
    endif()
    
    target_link_libraries(libi2pd       PUBLIC Boost::program_options)
    target_link_libraries(libi2pdclient PUBLIC Boost::program_options)
    target_link_libraries(libi2pdlang   PUBLIC                       )
    
    if (ANDROID)
        target_compile_definitions(i2pd PRIVATE ANDROID_BINARY=1)
    endif()
endif()

################################################################################
project(ouinet_i2p)

add_library(ouinet_i2p OBJECT
    "./address.cpp"
    "./address.cpp"
    "./sam.cpp"
    "./i2pd.cpp"
    "./session.cpp"
    "./session_id.cpp"
    "./tracker.cpp"
    "./tracker_lookup.cpp"
    "./announcer.cpp"
    "./util/create_i2p_session.cpp"
)

ouinet_setup(TARGET ouinet_i2p
    LINK ouinet_asio
    DEFS
        OUINET_I2P_EXPORT
        OUINET_COMMON_NOAPI
)

if (WITH_I2PD)
    add_dependencies(ouinet_i2p i2pd)
endif()

target_include_directories(ouinet_i2p INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

################################################################################

# Place the resulting binaries at the root of the build directory.
set_target_properties(ouinet_i2p PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if (WITH_I2PD)
    set_target_properties(i2pd PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

################################################################################
if (WITH_I2PD AND ANDROID)
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
    target_link_libraries(libi2pd PRIVATE ifaddrs)
endif()
