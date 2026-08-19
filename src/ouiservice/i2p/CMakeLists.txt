cmake_minimum_required (VERSION 3.13)

include(ExternalProject)

################################################################################
if (WITH_I2PD_EXE OR WITH_I2PD_LIB)
    set(WITH_I2PD TRUE)
endif()

if (WITH_I2PD)
    block()
        if (WITH_I2PD_EXE)
            set(WITH_BINARY ON)
        else(WITH_I2PD_LIB)
            set(WITH_LIBRARY ON)
        endif()
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
    
    if (MINGW)                                                                                                                                          
        # mingw-w64 GCC's libstdc++.a (tinfo.o) and Boost's c++23 object files                                                                                          
        # both emit a non-weak definition of std::type_info::operator==,                                                                                                
        # causing a "multiple definition" link error (GCC bug 110572). Both                                                                                             
        # definitions are identical implementations of the same standard                                                                                                
        # function, so it's safe to let the linker keep either one.                                                                                                     
        target_link_options(i2pd PRIVATE -Wl,--allow-multiple-definition)                                                                                               
        set(MINGW_EXTRA iphlpapi)
    endif()     

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
    
    target_link_libraries(libi2pd       PUBLIC Boost::program_options ${MINGW_EXTRA})
    target_link_libraries(libi2pdclient PUBLIC Boost::program_options Boost::json ${MINGW_EXTRA})
    target_link_libraries(libi2pdlang   PUBLIC                       )
    
    if (ANDROID)
        target_compile_definitions(i2pd PRIVATE ANDROID_BINARY=1)
    endif()
endif()

################################################################################
project(ouinet_i2p)

set(ouinet_i2p_defs
    OUINET_API_I2P_EXPORT
    OUINET_API_COMMON_LOCAL
)

if (ANDROID)
    list(APPEND ouinet_i2p_defs PRIVATE ANDROID_BINARY=1)
endif()

set(ouinet_i2p_libs
    ouinet_asio
)

set(ouinet_i2p_includes)

set(ouinet_i2p_cpps
    address.cpp
    address.cpp
    sam.cpp
    i2pd.cpp
    session.cpp
    session_id.cpp
    tracker.cpp
    tracker_lookup.cpp
    announcer.cpp
    service.cpp
)

if (WITH_I2PD_EXE)
    list(APPEND ouinet_i2p_defs PUBLIC OUINET_WITH_I2PD_EXE)
    list(APPEND ouinet_i2p_libs Boost::process)
    list(APPEND ouinet_i2p_cpps i2pd_exe.cpp)
endif()

if(WITH_I2PD_LIB)
    list(APPEND ouinet_i2p_defs PUBLIC OUINET_WITH_I2PD_LIB)
    list(APPEND ouinet_i2p_libs libi2pd libi2pdclient libi2pdlang)
    list(APPEND ouinet_i2p_includes ./i2pd/daemon ./i2pd/libi2pd ./i2pd/libi2pd_client ./i2pd/i18n)
    list(APPEND ouinet_i2p_cpps
        i2pd_lib.cpp
        i2pd/daemon/Daemon.cpp
        i2pd/daemon/I2PControl.cpp
        i2pd/daemon/HTTPServer.cpp
        i2pd/daemon/I2PControlHandlers.cpp
    )
    if(UNIX)
        list(APPEND ouinet_i2p_cpps i2pd/daemon/UnixDaemon.cpp)
    elseif(WIN32 OR MINGW)
        list(APPEND ouinet_i2p_cpps
            i2pd/Win32/Win32App.cpp
            i2pd/Win32/Win32Service.cpp
            i2pd/Win32/Win32NetState.cpp
            i2pd/Win32/DaemonWin32.cpp
        )
    endif()
endif()

add_library(ouinet_i2p OBJECT ${ouinet_i2p_cpps})

ouinet_setup(TARGET ouinet_i2p
    LINK ${ouinet_i2p_libs} ${MINGW_EXTRA}
    DEFS ${ouinet_i2p_defs}
    INCS PRIVATE ${ouinet_i2p_includes}
)

if (WITH_I2PD_EXE)
    add_dependencies(ouinet_i2p i2pd)
endif()

################################################################################

# Place the resulting binaries at the root of the build directory.
set_target_properties(ouinet_i2p PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if (WITH_I2PD_EXE)
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
