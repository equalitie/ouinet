set(BOOST_VERSION 1.71.0)
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

string(REPLACE "." "_" BOOST_VERSION_FILENAME ${BOOST_VERSION})

set(BOOST_PATCHES
    ${CMAKE_CURRENT_LIST_DIR}/inline-boost/beast-header-parser-fix-${BOOST_VERSION_FILENAME}.patch
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    get_filename_component(COMPILER_TOOLCHAIN_PREFIX ${_CMAKE_TOOLCHAIN_PREFIX} NAME)
    string(REGEX REPLACE "-$" "" COMPILER_HOSTTRIPLE ${COMPILER_TOOLCHAIN_PREFIX})
    # This is the same as COMPILER_HOSTTRIPLE, _except_ on arm32.
    set(COMPILER_CC_PREFIX ${COMPILER_HOSTTRIPLE})

    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(COMPILER_CC_PREFIX "armv7a-linux-androideabi")
        set(BOOST_ARCH "armeabiv7a")
        set(BOOST_ABI "aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(BOOST_ARCH "arm64v8a")
        set(BOOST_ABI "aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(BOOST_ARCH "x86")
        set(BOOST_ABI "sysv")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(BOOST_ARCH "x8664")
        set(BOOST_ABI "sysv")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(BOOST_PATCHES ${BOOST_PATCHES} ${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-android-${BOOST_VERSION_FILENAME}.patch)
    set(BOOST_ENVIRONMENT
        export
            PATH=${COMPILER_DIR}:$ENV{PATH}
            BOOSTARCH=${BOOST_ARCH}
            BINUTILS_PREFIX=${COMPILER_DIR}/${COMPILER_HOSTTRIPLE}-
            COMPILER_FULL_PATH=${COMPILER_DIR}/${COMPILER_CC_PREFIX}${ANDROID_PLATFORM_LEVEL}-clang++
        &&
    )
    set(BOOST_ARCH_CONFIGURATION
        --user-config=${CMAKE_CURRENT_LIST_DIR}/inline-boost/user-config-android.jam
        target-os=android
        toolset=clang-${BOOST_ARCH}
        abi=${BOOST_ABI}
    )
else()
    set(BOOST_ENVIRONMENT )
    set(BOOST_ARCH_CONFIGURATION )
endif()

set(BUILT_BOOST_VERSION ${BOOST_VERSION})
set(BUILT_BOOST_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/boost/install/include)
set(BUILT_BOOST_LIBRARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/boost/install/lib)
set(BUILT_BOOST_COMPONENTS ${BOOST_COMPONENTS})

function(_boost_library_filename component output_var)
    set(${output_var} "${BUILT_BOOST_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}boost_${component}${CMAKE_STATIC_LIBRARY_SUFFIX}" PARENT_SCOPE)
endfunction(_boost_library_filename)

include(${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-dependencies.cmake)
_static_Boost_recursive_dependencies("${BOOST_COMPONENTS}" BOOST_DEPENDENT_COMPONENTS)
set(ENABLE_BOOST_COMPONENTS )
set(BOOST_LIBRARY_FILES )
foreach (component ${BOOST_DEPENDENT_COMPONENTS})
    if (${component} STREQUAL "unit_test_framework")
        set(ENABLE_BOOST_COMPONENTS ${ENABLE_BOOST_COMPONENTS} --with-test)
        continue()
    endif()
    set(ENABLE_BOOST_COMPONENTS ${ENABLE_BOOST_COMPONENTS} --with-${component})
    _boost_library_filename(${component} filename)
    set(BOOST_LIBRARY_FILES ${BOOST_LIBRARY_FILES} ${filename})
endforeach()

set(BOOST_PATCH_COMMAND
    cd ${CMAKE_CURRENT_BINARY_DIR}/boost/src/built_boost
)
foreach (patch ${BOOST_PATCHES})
    set(BOOST_PATCH_COMMAND ${BOOST_PATCH_COMMAND} && patch -p1 -i ${patch})
endforeach()

externalproject_add(built_boost
    URL "https://boostorg.jfrog.io/artifactory/main/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_FILENAME}.tar.bz2""
    URL_HASH SHA256=d73a8da01e8bf8c7eda40b4c84915071a8c8a0df4a6734537ddde4a8580524ee
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/boost"
    PATCH_COMMAND ${BOOST_PATCH_COMMAND}
    CONFIGURE_COMMAND
           cd ${CMAKE_CURRENT_BINARY_DIR}/boost/src/built_boost
        && ./bootstrap.sh
    BUILD_COMMAND
           cd ${CMAKE_CURRENT_BINARY_DIR}/boost/src/built_boost
        && ${BOOST_ENVIRONMENT} ./b2
            link=static
            threading=multi
            -j2
            --layout=system
            --prefix=${CMAKE_CURRENT_BINARY_DIR}/boost/install
            --no-cmake-config
            ${ENABLE_BOOST_COMPONENTS}
            ${BOOST_ARCH_CONFIGURATION}
            install
    BUILD_BYPRODUCTS ${BOOST_LIBRARY_FILES}
    INSTALL_COMMAND ""
)

set(Boost_DIR ${CMAKE_CURRENT_LIST_DIR}/inline-boost)
list(INSERT CMAKE_MODULE_PATH 0 ${Boost_DIR})


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
