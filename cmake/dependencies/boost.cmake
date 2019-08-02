set(BOOST_VERSION 1.67.0)
set(BOOST_COMPONENTS
    context
    coroutine
    date_time
    filesystem
    iostreams
    program_options
    regex
    system
    unit_test_framework
)


if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(BOOST_ARCH "armeabiv7a")
        set(BOOST_ARCH_SETTINGS "abi=aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "^arm.*")
        # Is this still relevant? armv<7 seems to be obsolete
        # from android 4.4 onwards.
        set(BOOST_ARCH "armeabi")
        set(BOOST_ARCH_SETTINGS "abi=aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(BOOST_ARCH "arm64v8a")
        set(BOOST_ARCH_SETTINGS "abi=aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(BOOST_ARCH "x86")
        set(BOOST_ARCH_SETTINGS "abi=sysv")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(BOOST_ARCH "x8664")
        set(BOOST_ARCH_SETTINGS "abi=sysv")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    string(REPLACE "." "_" BOOST_VERSION_FILENAME ${BOOST_VERSION})

    include(${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-dependencies.cmake)
    _static_Boost_recursive_dependencies("${BOOST_COMPONENTS}" BOOST_DEPENDENT_COMPONENTS)
    set(ENABLE_BOOST_COMPONENTS )
    foreach (component ${BOOST_DEPENDENT_COMPONENTS})
        if (${component} STREQUAL "unit_test_framework")
            set(ENABLE_BOOST_COMPONENTS ${ENABLE_BOOST_COMPONENTS} --with-test)
            continue()
        endif()
        set(ENABLE_BOOST_COMPONENTS ${ENABLE_BOOST_COMPONENTS} --with-${component})
    endforeach()

    externalproject_add(built_boost
        URL "https://sourceforge.net/projects/boost/files/boost/${BOOST_VERSION}/boost_${BOOST_VERSION_FILENAME}.tar.bz2"
        URL_MD5 ced776cb19428ab8488774e1415535ab
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/boost"
        PATCH_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/boost/src/built_boost
            && patch -p1 -i ${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-${BOOST_VERSION_FILENAME}.patch
        CONFIGURE_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/boost/src/built_boost
            && ./bootstrap.sh
        BUILD_COMMAND
               cd ${CMAKE_CURRENT_BINARY_DIR}/boost/src/built_boost
            && export PATH=${CMAKE_ANDROID_STANDALONE_TOOLCHAIN}/bin:$ENV{PATH}
            && export CLANGPATH=${CMAKE_ANDROID_STANDALONE_TOOLCHAIN}/bin
            && export BOOSTARCH=${BOOST_ARCH}
            && ./b2
                target-os=android
                toolset=clang-${BOOST_ARCH}
                link=static
                threading=multi
                --layout=system
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/boost/install
                --user-config=${CMAKE_CURRENT_LIST_DIR}/inline-boost/user-config.jam
                --no-cmake-config
                ${ENABLE_BOOST_COMPONENTS}
                ${BOOST_ARCH_SETTINGS}
                install
        INSTALL_COMMAND ""
    )

    set(BUILT_BOOST_VERSION ${BOOST_VERSION})
    set(BUILT_BOOST_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/boost/install/include)
    set(BUILT_BOOST_LIBRARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/boost/install/lib)
    set(BUILT_BOOST_COMPONENTS ${BOOST_COMPONENTS})

    set(Boost_DIR ${CMAKE_CURRENT_LIST_DIR}/inline-boost)
    list(INSERT CMAKE_MODULE_PATH 0 ${Boost_DIR})
endif()


find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS ${BOOST_COMPONENTS})
find_package(Threads REQUIRED)


if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # Boost.Beast has a patch that uses the ANDROID definition
    # instead of the implicit __ANDROID__ one.
    # https://github.com/boostorg/beast/blob/44f37d1a11d/include/boost/beast/core/impl/file_posix.ipp#L27
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "ANDROID"
    )
endif()


# gcc 8 spits out warnings from Boost.Mpl about unnecessary parentheses
# https://github.com/CauldronDevelopmentLLC/cbang/issues/26
# TODO: Perhaps do a check for Boost and gcc version before adding this flag?
set_target_properties(Boost::boost PROPERTIES
    INTERFACE_COMPILE_OPTIONS "-Wno-parentheses"
)


# Asio can be used as a header-only library _only_ if it is not used along any
# shared library boundaries. In any program using asio across a library
# boundary, both the library and the program must link asio as a shared library
# instead. Boost does not ship this library, so we need to create it.

add_library(boost_asio SHARED "${CMAKE_CURRENT_SOURCE_DIR}/lib/asio.cpp")
add_library(Boost::asio ALIAS boost_asio)
target_link_libraries(boost_asio
    PUBLIC
        Boost::boost
        Boost::coroutine
        Threads::Threads
    PRIVATE
        Boost::system
)
target_compile_definitions(boost_asio
    PUBLIC
        -DBOOST_ASIO_DYN_LINK
        # For some reason we need to define both of these
        -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
        -DBOOST_COROUTINE_NO_DEPRECATION_WARNING
)
target_compile_options(boost_asio
    PUBLIC -std=c++14
)

add_library(boost_asio_ssl SHARED "${CMAKE_CURRENT_SOURCE_DIR}/lib/asio_ssl.cpp")
add_library(Boost::asio_ssl ALIAS boost_asio_ssl)
target_link_libraries(boost_asio_ssl
    PUBLIC
        OpenSSL::SSL
        Boost::asio
)


# FindBoost.cmake doesn't define targets for newer versions of boost.
# Let's emulate it instead.
foreach(component ${BOOST_COMPONENTS})
    if (NOT TARGET Boost::${component})
        include(${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-dependencies.cmake)
        _static_Boost_recursive_dependencies(${component} dependencies)

        find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS ${dependencies})
        list(GET Boost_LIBRARIES 0 imported_location)

        add_library(Boost::${component} UNKNOWN IMPORTED)
        set_target_properties(Boost::${component} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${Boost_LIBRARIES}"
            IMPORTED_LOCATION "${imported_location}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        )
    endif()
endforeach()
